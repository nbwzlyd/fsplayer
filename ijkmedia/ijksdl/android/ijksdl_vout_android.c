/*
 * ijksdl_vout_android.c
 *
 * 直出（zero-GPU）视频渲染器实现。
 *
 * 软解路径：AVFrame --sws_scale--> RGBA --> ANativeWindow_lock/unlockAndPost
 * 硬解路径：不经过本文件（MediaCodec 直接把 buffer 投到 SurfaceView 的 Surface）
 *
 * 本文件不含任何 EGL / GLES / Vulkan 调用。
 */

#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "ijksdl/android/ijksdl_vout_android.h"
#include "ijksdl/ijksdl_vout_internal.h"
#include "ijksdl/ijksdl_log.h"
#include "ijksdl/ijksdl_mutex.h"
#include "ijksdl/ffmpeg/ijksdl_inc_ffmpeg.h"

#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "ijkplayer/ff_ffplay_def.h"

#define TAG "SDL_VoutAndroid"

/* 临时诊断：Z6 上 logcat 不可用，native 直接写文件（App 自己的外部私有目录） */
#define FSDBG_PATH "/sdcard/Android/data/com.huawei.himovceie/files/fsnative.txt"
void SDL_VoutAndroid_DebugLog(const char *fmt, ...)
{
    static int seq = 0;
    FILE *f = fopen(FSDBG_PATH, seq == 0 ? "w" : "a");
    seq++;
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}


/* 与 iOS / 旧 Vulkan 渲染器一致的缩放模式取值 */
enum {
    FSScalingModeFit     = 0,   /* 等比完整显示（letterbox） */
    FSScalingModeFill    = 1,   /* 等比铺满（裁剪） */
    FSScalingModeStretch = 2,   /* 非等比拉伸 */
};

typedef struct SDL_Vout_Opaque {
    ANativeWindow *window;
    jobject        jsurface;   /* android/view/Surface 的 global ref（MediaCodec 必须用它） */
    int            win_w;
    int            win_h;
    /* 软解路径当前设置过的窗口缓冲几何，避免每帧重复 setBuffersGeometry */
    int            geom_w;
    int            geom_h;

    int            scaling_mode;
    int            bg_r, bg_g, bg_b;

    float          brightness, saturation, contrast;

    float          x_rotate, y_rotate, z_rotate;

    struct SwsContext *sws;
    int            sws_src_w, sws_src_h, sws_src_fmt;
    int            sws_dst_w, sws_dst_h;

    /* 临时缓冲：仅在需要裁剪（FILL）或旋转 90/270 时使用 */
    uint8_t       *tmp;
    size_t         tmp_size;

    /* 色彩调整 LUT（只在需要时构建） */
    uint8_t        lut_r[256], lut_g[256], lut_b[256];
    int            lut_valid;

    /* 快照 */
    uint8_t       *snap;
    size_t         snap_size;
    int            snap_w, snap_h;
    int            snap_pending;

    /* 诊断 */
    int            diag_pixfmt;
    int            diag_10bit;
    int            diag_path;
    int            diag_gate;

    int            direct_active;
    int            hdr_content;
    int            hdr_allow;
    int            logged_rotate;
} SDL_Vout_Opaque;

/* ------------------------------------------------------------------ */
/* overlay：只作为帧队列的“令牌”，不复制像素                             */
/* ------------------------------------------------------------------ */

typedef struct SDL_VoutOverlay_Token {
    int width;
    int height;
    int format;
} SDL_VoutOverlay_Token;

static int token_fill_frame(SDL_VoutOverlay *overlay, const AVFrame *frame)
{
    /* 显示时直接消费 Frame->frame，不做任何像素拷贝 */
    (void) overlay;
    (void) frame;
    return 0;
}

static int token_lock(SDL_VoutOverlay *overlay) { (void) overlay; return 0; }
static int token_unlock(SDL_VoutOverlay *overlay) { (void) overlay; return 0; }

static void token_free_l(SDL_VoutOverlay *overlay)
{
    if (!overlay)
        return;
    free(overlay->opaque);
    free(overlay);
}

static SDL_VoutOverlay *vout_create_overlay(int width, int height, int src_format, SDL_Vout *vout)
{
    SDL_VoutOverlay *overlay = SDL_VoutOverlay_CreateInternal(sizeof(SDL_VoutOverlay_Token));
    if (!overlay)
        return NULL;

    SDL_VoutOverlay_Token *token = overlay->opaque;
    token->width  = width;
    token->height = height;

    token->format = src_format;
    overlay->free_l = token_free_l;
    overlay->lock = token_lock;
    overlay->unlock = token_unlock;
    overlay->func_fill_frame = token_fill_frame;

    (void) vout;
    return overlay;
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static int format_is_10bit(int fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV422P10LE:
        case AV_PIX_FMT_YUV444P10LE:
        case AV_PIX_FMT_YUV420P10BE:
        case AV_PIX_FMT_P010LE:
        case AV_PIX_FMT_P010BE:
            return 1;
        default:
            return 0;
    }
}

static void build_color_lut(SDL_Vout_Opaque *opaque)
{
    float b = opaque->brightness;
    float c = opaque->contrast;
    if (b <= 0.0f) b = 1.0f;
    if (c <= 0.0f) c = 1.0f;
    for (int i = 0; i < 256; i++) {
        float v = ((float) i - 128.0f) * c + 128.0f * b;
        int iv = (int) (v + 0.5f);
        if (iv < 0) iv = 0;
        if (iv > 255) iv = 255;
        opaque->lut_r[i] = (uint8_t) iv;
        opaque->lut_g[i] = (uint8_t) iv;
        opaque->lut_b[i] = (uint8_t) iv;
    }
    opaque->lut_valid = 1;
}

/* 只做亮度/对比度（线性），饱和度为 1 时走这条最快的路 */
static void apply_lut_inplace(uint8_t *pixels, int stride, int w, int h, const SDL_Vout_Opaque *opaque)
{
    for (int y = 0; y < h; ++y) {
        uint8_t *row = pixels + (size_t) y * stride;
        for (int x = 0; x < w; ++x) {
            row[0] = opaque->lut_r[row[0]];
            row[1] = opaque->lut_g[row[1]];
            row[2] = opaque->lut_b[row[2]];
            row += 4;
        }
    }
}

/* 带饱和度的完整调整：只在饱和度 != 1 时用 */
static void apply_lut_sat_inplace(uint8_t *pixels, int stride, int w, int h, const SDL_Vout_Opaque *opaque)
{
    const float sat = opaque->saturation <= 0.0f ? 1.0f : opaque->saturation;
    for (int y = 0; y < h; ++y) {
        uint8_t *row = pixels + (size_t) y * stride;
        for (int x = 0; x < w; ++x) {
            float r = (float) opaque->lut_r[row[0]];
            float g = (float) opaque->lut_g[row[1]];
            float b = (float) opaque->lut_b[row[2]];
            float gray = 0.299f * r + 0.587f * g + 0.114f * b;
            float nr = gray + (r - gray) * sat;
            float ng = gray + (g - gray) * sat;
            float nb = gray + (b - gray) * sat;
            int ir = (int) (nr + 0.5f), ig = (int) (ng + 0.5f), ib = (int) (nb + 0.5f);
            row[0] = (uint8_t) (ir < 0 ? 0 : (ir > 255 ? 255 : ir));
            row[1] = (uint8_t) (ig < 0 ? 0 : (ig > 255 ? 255 : ig));
            row[2] = (uint8_t) (ib < 0 ? 0 : (ib > 255 ? 255 : ib));
            row += 4;
        }
    }
}

static int vout_ensure_tmp(SDL_Vout_Opaque *opaque, size_t need)
{
    if (opaque->tmp_size >= need && opaque->tmp)
        return 0;
    free(opaque->tmp);
    opaque->tmp = (uint8_t *) malloc(need);
    if (!opaque->tmp) {
        opaque->tmp_size = 0;
        return -1;
    }
    opaque->tmp_size = need;
    return 0;
}

/* 把 src（RGBA, src_stride）旋转 90/180/270 后写入 dst 里的目标区域 */
static void blit_rotated(uint8_t *dst_base, int dst_stride, int dst_x, int dst_y,
                         int dst_w, int dst_h,
                         const uint8_t *src, int src_stride, int src_w, int src_h,
                         int rotate)
{
    /* rotate: 90 => 顺时针 90；270 => 逆时针 90 */
    if (rotate == 180) {
        for (int y = 0; y < src_h; ++y) {
            const uint8_t *srow = src + (size_t) (src_h - 1 - y) * src_stride;
            uint8_t *drow = dst_base + (size_t) (dst_y + y) * dst_stride + (size_t) dst_x * 4;
            for (int x = 0; x < src_w; ++x) {
                const uint8_t *sp = srow + (size_t) (src_w - 1 - x) * 4;
                drow[0] = sp[0]; drow[1] = sp[1]; drow[2] = sp[2]; drow[3] = 255;
                drow += 4;
            }
        }
    } else if (rotate == 90) {
        /* 输出宽 = src_h，输出高 = src_w */
        for (int y = 0; y < src_h; ++y) {
            const uint8_t *srow = src + (size_t) y * src_stride;
            for (int x = 0; x < src_w; ++x) {
                const uint8_t *sp = srow + (size_t) x * 4;
                int dx = src_h - 1 - y;
                int dy = x;
                if (dx >= 0 && dx < dst_w && dy >= 0 && dy < dst_h) {
                    uint8_t *dp = dst_base + (size_t) (dst_y + dy) * dst_stride
                                  + (size_t) (dst_x + dx) * 4;
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                }
            }
        }
    } else if (rotate == 270) {
        for (int y = 0; y < src_h; ++y) {
            const uint8_t *srow = src + (size_t) y * src_stride;
            for (int x = 0; x < src_w; ++x) {
                const uint8_t *sp = srow + (size_t) x * 4;
                int dx = y;
                int dy = src_w - 1 - x;
                if (dx >= 0 && dx < dst_w && dy >= 0 && dy < dst_h) {
                    uint8_t *dp = dst_base + (size_t) (dst_y + dy) * dst_stride
                                  + (size_t) (dst_x + dx) * 4;
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 显示                                                                */
/* ------------------------------------------------------------------ */

static int vout_display_overlay_l(SDL_Vout *vout, const Frame *frame, SDL_TextureOverlay *sub_overlay)
{
    SDL_Vout_Opaque *opaque = vout->opaque;
    if (!opaque || !opaque->window) {
        static int n_nowin = 0;
        if ((n_nowin++ % 50) == 0)
            SDL_VoutAndroid_DebugLog("no-window n=%d", n_nowin - 1);
        return 0;
    }

    (void) sub_overlay; /* 字幕由 Java 层绘制，这里不处理 */

    AVFrame *av_frame = (frame && frame->frame) ? frame->frame : NULL;
    if (!av_frame)
        return 0; /* 纯音频：无需上屏 */

    /* 硬解直出：MediaCodec 已经直接把 buffer 投到 Surface，这里什么都不做 */
    if (av_frame->format == AV_PIX_FMT_MEDIACODEC) {
        static int n_mc = 0;
        opaque->diag_path = 1;
        opaque->diag_pixfmt = av_frame->format;
        if ((n_mc++ % 25) == 0)
            SDL_VoutAndroid_DebugLog("mcdisp n=%d win=%p w=%d", n_mc - 1, (void *) opaque->window,
                                     ANativeWindow_getWidth(opaque->window));
        return 0;
    }
    {
        static int n_soft = 0;
        if ((n_soft++ % 25) == 0)
            SDL_VoutAndroid_DebugLog("softdisp n=%d fmt=%d win=%p", n_soft - 1, av_frame->format,
                                     (void *) opaque->window);
    }

    opaque->diag_pixfmt = av_frame->format;
    opaque->diag_path = 2;
    opaque->diag_10bit = format_is_10bit(av_frame->format);
    if (opaque->diag_gate == 0)
        opaque->diag_gate = 99;
    opaque->hdr_content = (av_frame->colorspace == AVCOL_SPC_BT2020_NCL
                           || av_frame->colorspace == AVCOL_SPC_BT2020_CL) ? 1 : 0;

    if (av_frame->width <= 0 || av_frame->height <= 0)
        return -1;

    int win_w = ANativeWindow_getWidth(opaque->window);
    int win_h = ANativeWindow_getHeight(opaque->window);
    if (win_w <= 0 || win_h <= 0)
        return 0;
    opaque->win_w = win_w;
    opaque->win_h = win_h;

    int disp_w = (frame->disp_w > 0) ? frame->disp_w : av_frame->width;
    int disp_h = (frame->disp_h > 0) ? frame->disp_h : av_frame->height;
    int sar_n = (frame->sar.num > 0) ? frame->sar.num : 1;
    int sar_d = (frame->sar.den > 0) ? frame->sar.den : 1;

    int rotate = 0;
    int rz = (int) lroundf(opaque->z_rotate);
    rz = ((rz % 360) + 360) % 360;
    if (rz == 90 || rz == 180 || rz == 270)
        rotate = rz;
    else if (rz != 0 && !opaque->logged_rotate) {
        ALOGW("%s: unsupported z rotate %d, fallback to 0", TAG, rz);
        opaque->logged_rotate = 1;
    }

    /* 旋转 90/270 时，画面宽高互换 */
    int pic_w = disp_w * sar_n;
    int pic_h = disp_h * sar_d;
    if (rotate == 90 || rotate == 270) {
        int t = pic_w;
        pic_w = pic_h;
        pic_h = t;
    }
    if (pic_w <= 0 || pic_h <= 0)
        return -1;

    /* 计算目标矩形 */
    int dst_w, dst_h, dst_x, dst_y;
    if (opaque->scaling_mode == FSScalingModeStretch) {
        dst_w = win_w;
        dst_h = win_h;
        dst_x = 0;
        dst_y = 0;
    } else {
        double pic_aspect   = (double) pic_w / (double) pic_h;
        double win_aspect   = (double) win_w / (double) win_h;
        if (opaque->scaling_mode == FSScalingModeFill) {
            /* 铺满：取较大的缩放比，超出部分裁掉 */
            if (win_aspect > pic_aspect) {
                dst_w = win_w;
                dst_h = (int) lround(win_w / pic_aspect);
            } else {
                dst_h = win_h;
                dst_w = (int) lround(win_h * pic_aspect);
            }
        } else {
            /* 完整显示：取较小的缩放比，留黑边 */
            if (win_aspect > pic_aspect) {
                dst_h = win_h;
                dst_w = (int) lround(win_h * pic_aspect);
            } else {
                dst_w = win_w;
                dst_h = (int) lround(win_w / pic_aspect);
            }
        }
        dst_x = (win_w - dst_w) / 2;
        dst_y = (win_h - dst_h) / 2;
    }
    if (dst_w <= 0 || dst_h <= 0)
        return -1;

    /* 旋转后的“未旋转源尺寸” */
    int src_scale_w = dst_w, src_scale_h = dst_h;
    if (rotate == 90 || rotate == 270) {
        src_scale_w = dst_h;
        src_scale_h = dst_w;
    }

    opaque->sws = sws_getCachedContext(opaque->sws,
                                       av_frame->width, av_frame->height,
                                       (enum AVPixelFormat) av_frame->format,
                                       src_scale_w, src_scale_h,
                                       AV_PIX_FMT_RGBA,
                                       SWS_BILINEAR | SWS_ACCURATE_RND,
                                       NULL, NULL, NULL);
    if (!opaque->sws) {
        ALOGE("%s: sws_getCachedContext failed fmt=%d %dx%d -> %dx%d",
              TAG, av_frame->format, av_frame->width, av_frame->height, src_scale_w, src_scale_h);
        return -1;
    }
    opaque->sws_src_w = av_frame->width;
    opaque->sws_src_h = av_frame->height;
    opaque->sws_src_fmt = av_frame->format;
    opaque->sws_dst_w = src_scale_w;
    opaque->sws_dst_h = src_scale_h;

    const uint8_t *src_slice[4] = { av_frame->data[0], av_frame->data[1],
                                    av_frame->data[2], av_frame->data[3] };
    int src_stride[4] = { av_frame->linesize[0], av_frame->linesize[1],
                          av_frame->linesize[2], av_frame->linesize[3] };

    int need_tmp = (rotate != 0) || (opaque->scaling_mode == FSScalingModeFill);
    int has_color_adjust = (fabsf(opaque->brightness - 1.0f) > 0.001f
                            || fabsf(opaque->contrast - 1.0f) > 0.001f
                            || fabsf(opaque->saturation - 1.0f) > 0.001f);

    if (opaque->geom_w != win_w || opaque->geom_h != win_h) {
        ANativeWindow_setBuffersGeometry(opaque->window, win_w, win_h, WINDOW_FORMAT_RGBA_8888);
        opaque->geom_w = win_w;
        opaque->geom_h = win_h;
    }

    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(opaque->window, &buf, NULL) != 0) {
        ALOGW("%s: ANativeWindow_lock failed", TAG);
        return -1;
    }
    if (!buf.bits) {
        ANativeWindow_unlockAndPost(opaque->window);
        return -1;
    }

    uint8_t *base = (uint8_t *) buf.bits;
    int stride = buf.stride * 4;

    /* 背景色（黑边） */
    if (dst_w != win_w || dst_h != win_h) {
        if (opaque->bg_r == 0 && opaque->bg_g == 0 && opaque->bg_b == 0) {
            memset(base, 0, (size_t) stride * win_h);
        } else {
            uint32_t c = ((uint32_t) opaque->bg_r << 24) | ((uint32_t) opaque->bg_g << 16)
                         | ((uint32_t) opaque->bg_b << 8) | 0xff;
            for (int y = 0; y < win_h; ++y) {
                uint32_t *row = (uint32_t *) (base + (size_t) y * stride);
                for (int x = 0; x < win_w; ++x)
                    row[x] = c;
            }
        }
    }

    if (!need_tmp) {
        uint8_t *p_dst = base + (size_t) dst_y * stride + (size_t) dst_x * 4;
        uint8_t *dst_slice[4] = { p_dst, NULL, NULL, NULL };
        int dst_stride_arr[4] = { stride, 0, 0, 0 };
        sws_scale(opaque->sws, src_slice, src_stride, 0, av_frame->height,
                  dst_slice, dst_stride_arr);
        if (has_color_adjust) {
            if (!opaque->lut_valid)
                build_color_lut(opaque);
            if (fabsf(opaque->saturation - 1.0f) > 0.001f)
                apply_lut_sat_inplace(p_dst, stride, dst_w, dst_h, opaque);
            else
                apply_lut_inplace(p_dst, stride, dst_w, dst_h, opaque);
        }
        if (opaque->snap_pending) {
            size_t need = (size_t) dst_w * dst_h * 4;
            if (opaque->snap_size < need) {
                free(opaque->snap);
                opaque->snap = (uint8_t *) malloc(need);
                opaque->snap_size = opaque->snap ? need : 0;
            }
            if (opaque->snap) {
                for (int y = 0; y < dst_h; ++y)
                    memcpy(opaque->snap + (size_t) y * dst_w * 4,
                           p_dst + (size_t) y * stride, (size_t) dst_w * 4);
                opaque->snap_w = dst_w;
                opaque->snap_h = dst_h;
            }
            opaque->snap_pending = 0;
        }
    } else {
        /* 先转到临时缓冲，再按需裁剪/旋转 */
        int render_w = (rotate == 90 || rotate == 270) ? dst_h : dst_w;
        int render_h = (rotate == 90 || rotate == 270) ? dst_w : dst_h;
        if (vout_ensure_tmp(opaque, (size_t) render_w * render_h * 4 + 64) != 0) {
            ANativeWindow_unlockAndPost(opaque->window);
            return -1;
        }
        int tmp_stride = render_w * 4;
        uint8_t *tmp_slice[4] = { opaque->tmp, NULL, NULL, NULL };
        int tmp_stride_arr[4] = { tmp_stride, 0, 0, 0 };
        sws_scale(opaque->sws, src_slice, src_stride, 0, av_frame->height,
                  tmp_slice, tmp_stride_arr);

        /* 裁剪到窗口内 */
        int vis_w = dst_w, vis_h = dst_h, off_x = dst_x, off_y = dst_y;
        int crop_left = 0, crop_top = 0;
        if (off_x < 0) { crop_left = -off_x; vis_w += off_x; off_x = 0; }
        if (off_y < 0) { crop_top = -off_y; vis_h += off_y; off_y = 0; }
        if (off_x + vis_w > win_w) vis_w = win_w - off_x;
        if (off_y + vis_h > win_h) vis_h = win_h - off_y;

        if (vis_w > 0 && vis_h > 0) {
            const uint8_t *cs = opaque->tmp + (size_t) crop_top * tmp_stride
                                + (size_t) crop_left * 4;
            blit_rotated(base, stride, off_x, off_y, vis_w, vis_h,
                         cs, tmp_stride, vis_w, vis_h, rotate);
        }
        opaque->snap_pending = 0;
    }

    if (!buf.bits) {
        ANativeWindow_unlockAndPost(opaque->window);
        return -1;
    }
    ANativeWindow_unlockAndPost(opaque->window);
    return 0;
}

static int vout_display_overlay(SDL_Vout *vout, const Frame *frame, SDL_TextureOverlay *sub_overlay)
{
    SDL_LockMutex(vout->mutex);
    int retval = vout_display_overlay_l(vout, frame, sub_overlay);
    SDL_UnlockMutex(vout->mutex);
    return retval;
}

static void vout_free_l(SDL_Vout *vout)
{
    if (!vout)
        return;

    SDL_Vout_Opaque *opaque = vout->opaque;
    if (opaque) {
        if (opaque->window) {
            ANativeWindow_release(opaque->window);
            opaque->window = NULL;
        }
        if (opaque->sws) {
            sws_freeContext(opaque->sws);
            opaque->sws = NULL;
        }
        free(opaque->tmp);
        opaque->tmp = NULL;
        opaque->tmp_size = 0;
        free(opaque->snap);
        opaque->snap = NULL;
        opaque->snap_size = 0;
    }

    SDL_Vout_FreeInternal(vout);
}

/* ------------------------------------------------------------------ */
/* 创建 / 公共 API                                                     */
/* ------------------------------------------------------------------ */

SDL_Vout *SDL_VoutAndroid_Create(void)
{
    SDL_Vout *vout = SDL_Vout_CreateInternal(sizeof(SDL_Vout_Opaque));
    if (!vout)
        return NULL;

    SDL_Vout_Opaque *opaque = vout->opaque;
    opaque->scaling_mode = FSScalingModeFit;
    opaque->bg_r = opaque->bg_g = opaque->bg_b = 0;
    opaque->brightness = 1.0f;
    opaque->saturation = 1.0f;
    opaque->contrast = 1.0f;
    opaque->hdr_allow = 1;
    opaque->diag_gate = 99;   /* 直出方案不再需要 Vulkan 门槛，恒为通过 */
    opaque->diag_path = 0;

    vout->create_overlay = vout_create_overlay;
    vout->free_l = vout_free_l;
    vout->display_overlay = vout_display_overlay;
    vout->overlay_format = 0;

    ALOGI("%s: direct (zero-GPU) vout created", TAG);
    return vout;
}

void SDL_VoutAndroid_SetAndroidSurface(JNIEnv *env, SDL_Vout *vout, jobject android_surface)
{
    if (!vout || !vout->opaque)
        return;

    SDL_LockMutex(vout->mutex);
    SDL_Vout_Opaque *opaque = vout->opaque;

    if (opaque->window) {
        ANativeWindow_release(opaque->window);
        opaque->window = NULL;
    }
    if (opaque->jsurface) {
        (*env)->DeleteGlobalRef(env, opaque->jsurface);
        opaque->jsurface = NULL;
    }

    if (android_surface) {
        opaque->window = ANativeWindow_fromSurface(env, android_surface);
        if (android_surface)
            opaque->jsurface = (*env)->NewGlobalRef(env, android_surface);
        if (opaque->window) {
            /*
             * 这里刻意不调 setBuffersGeometry：
             *  - 硬解直出时缓冲区由 MediaCodec 自己分配，强行把窗口格式改成
             *    RGBA_8888 会让 10bit/HDR 片源退化成 8bit，个别实现还会直接
             *    拒绝配置（Surface 格式不匹配）。
             *  - 软解路径在真正 lock 之前会按视频尺寸设置 RGBA_8888（见
             *    vout_display_overlay_l），两不冲突。
             */
            opaque->geom_w = 0;
            opaque->geom_h = 0;
            SDL_VoutAndroid_DebugLog("setSurface win=%p %dx%d", (void *) opaque->window,
                                     ANativeWindow_getWidth(opaque->window), ANativeWindow_getHeight(opaque->window));
            ALOGI("%s: surface set %dx%d", TAG,
                  ANativeWindow_getWidth(opaque->window), ANativeWindow_getHeight(opaque->window));
        } else {
            ALOGE("%s: ANativeWindow_fromSurface failed", TAG);
        }
    }

    SDL_UnlockMutex(vout->mutex);
}

int SDL_VoutAndroid_IsMediaCodecSupported(SDL_Vout *vout)
{
    /*
     * 直出：MediaCodec 直接把解码结果投到 SurfaceView 的 Surface。
     * 只要 Surface 已经建立就支持（不需要 AImageReader / Vulkan 外部显存）。
     */
    if (!vout || !vout->opaque)
        return 0;
    return vout->opaque->window ? 1 : 0;
}

jobject SDL_VoutAndroid_GetJavaSurface(SDL_Vout *vout)
{
    if (!vout || !vout->opaque)
        return NULL;
    return vout->opaque->jsurface;
}

ANativeWindow *SDL_VoutAndroid_GetNativeWindow(SDL_Vout *vout)
{
    if (!vout || !vout->opaque)
        return NULL;
    return vout->opaque->window;
}

int SDL_VoutAndroid_GetFSDiag(SDL_Vout *vout, int what)
{
    if (!vout || !vout->opaque)
        return -1;
    SDL_Vout_Opaque *opaque = vout->opaque;
    switch (what) {
        case 0: return opaque->diag_pixfmt;
        case 1: return opaque->diag_10bit;
        case 2: return opaque->diag_path;
        case 3: return opaque->diag_gate;
        case 4: return (opaque->hdr_content ? 4 : 0);
        default: return -1;
    }
}

jobject SDL_VoutAndroid_GetMediaCodecSurface(JNIEnv *env, SDL_Vout *vout)
{
    (void) env;
    (void) vout;
    return NULL;
}

SDL_GPU *SDL_VoutAndroid_GetGPU(SDL_Vout *vout)
{
    (void) vout;
    return NULL;
}

void SDL_VoutAndroid_SetScalingMode(SDL_Vout *vout, int mode)
{
    if (!vout || !vout->opaque)
        return;
    vout->opaque->scaling_mode = mode;
}

void SDL_VoutAndroid_SetRotateDegrees(SDL_Vout *vout, float x, float y, float z)
{
    if (!vout || !vout->opaque)
        return;
    vout->opaque->x_rotate = x;
    vout->opaque->y_rotate = y;
    vout->opaque->z_rotate = z;
}

void SDL_VoutAndroid_SetBackgroundImage(SDL_Vout *vout, const void *pixels, int width, int height)
{
    (void) vout; (void) pixels; (void) width; (void) height;
}

void SDL_VoutAndroid_SetBackgroundBlur(SDL_Vout *vout, int iterations, float sigma)
{
    (void) vout; (void) iterations; (void) sigma;
}

void SDL_VoutAndroid_SetColorAdjust(SDL_Vout *vout, float brightness, float saturation, float contrast)
{
    if (!vout || !vout->opaque)
        return;
    SDL_Vout_Opaque *opaque = vout->opaque;
    opaque->brightness = brightness;
    opaque->saturation = saturation;
    opaque->contrast = contrast;
    opaque->lut_valid = 0;
}

void SDL_VoutAndroid_SetBackgroundColor(SDL_Vout *vout, int red, int green, int blue)
{
    if (!vout || !vout->opaque)
        return;
    vout->opaque->bg_r = red;
    vout->opaque->bg_g = green;
    vout->opaque->bg_b = blue;
}

void SDL_VoutAndroid_SetAllowHDRDirectDisplay(SDL_Vout *vout, int allow)
{
    if (!vout || !vout->opaque)
        return;
    vout->opaque->hdr_allow = allow;
}

int SDL_VoutAndroid_IsDirectDisplayHDRSupported(SDL_Vout *vout)
{
    (void) vout;
    /* 由系统 display 负责 HDR 直出，native 侧不做探测 */
    return 0;
}

int SDL_VoutAndroid_IsHDRContent(SDL_Vout *vout)
{
    if (!vout || !vout->opaque)
        return 0;
    return vout->opaque->hdr_content;
}

int SDL_VoutAndroid_TakeSnapshot(SDL_Vout *vout, int type,
                                 int *out_w, int *out_h, void **out_pixels)
{
    (void) type;
    if (!vout || !vout->opaque || !out_pixels)
        return -1;

    SDL_Vout_Opaque *opaque = vout->opaque;
    if (opaque->direct_active)
        return -1; /* 硬解帧不在 CPU 侧 */

    SDL_LockMutex(vout->mutex);
    opaque->snap_pending = 1;
    SDL_UnlockMutex(vout->mutex);

    /* 等下一帧被转换出来（最多 ~1s） */
    for (int i = 0; i < 100; ++i) {
        SDL_LockMutex(vout->mutex);
        int done = !opaque->snap_pending;
        int w = opaque->snap_w, h = opaque->snap_h;
        SDL_UnlockMutex(vout->mutex);
        if (done && w > 0 && h > 0 && opaque->snap) {
            size_t need = (size_t) w * h * 4;
            void *copy = malloc(need);
            if (!copy)
                return -1;
            memcpy(copy, opaque->snap, need);
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            *out_pixels = copy;
            return 0;
        }
        usleep(10 * 1000);
    }
    return -1;
}

void SDL_VoutAndroid_SetDirectDecodeActive(SDL_Vout *vout, int active)
{
    if (!vout || !vout->opaque)
        return;
    vout->opaque->direct_active = active ? 1 : 0;
    vout->opaque->diag_path = active ? 1 : 0;
}

/*
 * 直出方案不再创建 SDL_GPU（字幕改由 Java 层绘制），
 * 但 ffplayer 销毁路径仍会调用 SDL_GPUFreeP；Vulkan 实现已不再参与编译，
 * 这里补一个空实现避免链接失败。
 */
void SDL_GPUFreeP(SDL_GPU **pgpu)
{
    if (pgpu)
        *pgpu = NULL;
}
