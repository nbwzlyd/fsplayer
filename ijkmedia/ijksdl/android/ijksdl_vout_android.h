/*****************************************************************************
 * ijksdl_vout_android.h
 *****************************************************************************
 *
 * Copyright (c) 2019 debugly <qianlongxu@gmail.com>
 *
 * This file is part of FSPlayer.
 *
 * FSPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * FSPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FSPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef IJKSDL_ANDROID__IJKSDL_VOUT_ANDROID_H
#define IJKSDL_ANDROID__IJKSDL_VOUT_ANDROID_H

#include <jni.h>
#include <android/native_window.h>
#include "../ijksdl_stdinc.h"
#include "../ijksdl_vout.h"
#include "../ijksdl_gpu.h"

/*
 * 安卓「直出」视频渲染器（替代已废弃的 Vulkan 方案）。
 *
 * 设计目标（对齐 media3）：显存占用为 0，不引入任何 3D 图形栈。
 *
 * 硬解（主路径）：MediaCodec 的 ANativeWindow 输出 Surface 直接就是 SurfaceView
 *   的 Surface，解码器 producer 直接投递到 display，CPU/GPU 都不搬运一帧数据。
 *   这条路上本文件只提供 surface 与诊断信息，真正上屏由
 *   ffpipenode_android_mediacodec_direct.c 负责。
 *
 * 软解（兜底路径）：FFmpeg 软解出的 AVFrame 由本文件用 libswscale 转成 RGBA，
 *   再通过 ANativeWindow_lock/unlockAndPost 拷进显示缓冲。没有 EGL、没有纹理、
 *   没有 swapchain，显存占用同样为 0（只占一块 surface 的后备缓冲）。
 *
 * 字幕：不再走 GPU 纹理叠加（那需要 3D 上下文）。字幕由 native 侧生成 RGBA 位图
 *   并通过 Java 回调交给 App，由 Java 层 View 绘制 —— 与 media3 的字幕交付方式一致。
 */

/* 创建直出渲染器。 */
SDL_Vout *SDL_VoutAndroid_Create(void);

/* 由上层 (ijkmp_android_set_surface) 调用，把 Java Surface 传给渲染器。 */
void SDL_VoutAndroid_SetAndroidSurface(JNIEnv *env, SDL_Vout *vout, jobject android_surface);

/*
 * 直出模式下没有 AImageReader 回读通路，返回 0。
 * 上层据此不再尝试 FFmpeg 的 *_mediacodec 硬件解码器，改走自管 MediaCodec 直出节点。
 */
int SDL_VoutAndroid_IsMediaCodecSupported(SDL_Vout *vout);

/* 「播放信息」面板用的诊断位，what 含义见 fs_vulkan_renderer_diag 的旧约定：
 * 0=AVPixelFormat 1=是否10bit 2=通路(1=mc零拷贝 2=cpu-yuv 0=未知) 3=硬解门槛 4=色彩打包 */
int SDL_VoutAndroid_GetFSDiag(SDL_Vout *vout, int what);
void SDL_VoutAndroid_ResetWindowGeometry(SDL_Vout *vout);

/*
 * 借出 SurfaceView 的 ANativeWindow（生命周期由 vout 持有，调用方不要 release）。
 * 硬解直出时把它交给 MediaCodec 作为解码输出目标。
 */
ANativeWindow *SDL_VoutAndroid_GetNativeWindow(SDL_Vout *vout);

/* MediaCodec 的 configure 只认 android/view/Surface；传 ANativeWindow 会被当成 ByteBuffer 模式 */
jobject SDL_VoutAndroid_GetJavaSurface(SDL_Vout *vout);

/* 临时诊断：写文件（logcat 在部分机型不可用）。
 * 默认【关闭】——release 包零 IO；排查时由 Java 侧 FSPlayer.setDebugLogEnabled(true) 打开。
 * 路径必须由 Java 用 setDebugLogPath(getExternalFilesDir(null)+"/fsnative.txt") 传入：
 * 同一份 .so 被 TV / Mobile 两个包名共用，不能在这里写死包名。 */
void SDL_VoutAndroid_DebugLog(const char *fmt, ...);
void SDL_VoutAndroid_SetDebugLogEnabled(int enabled);
int  SDL_VoutAndroid_GetDebugLogEnabled(void);
void SDL_VoutAndroid_SetDebugLogPath(const char *path);

/* 直出模式不提供回读 Surface（返回 NULL）。 */
jobject SDL_VoutAndroid_GetMediaCodecSurface(JNIEnv *env, SDL_Vout *vout);

/* 不再有 GPU 上下文（字幕改由 Java 层绘制），恒返回 NULL。 */
SDL_GPU *SDL_VoutAndroid_GetGPU(SDL_Vout *vout);

/* 设置画面缩放模式：0 等比完整显示（默认）1 等比铺满 2 非等比拉伸。播放中随时可调。 */
void SDL_VoutAndroid_SetScalingMode(SDL_Vout *vout, int mode);

/* 设置画面手动三轴旋转（度）。仅软解路径支持 Z 轴 90/180/270。 */
void SDL_VoutAndroid_SetRotateDegrees(SDL_Vout *vout, float x, float y, float z);

/* 背景图/模糊：直出模式无 GPU 上下文，忽略（软解路径只支持纯色背景）。 */
void SDL_VoutAndroid_SetBackgroundImage(SDL_Vout *vout, const void *pixels, int width, int height);
void SDL_VoutAndroid_SetBackgroundBlur(SDL_Vout *vout, int iterations, float sigma);

/* 色彩调整（亮度/饱和度/对比度），默认都是 1.0；只作用于软解路径的视频画面。 */
void SDL_VoutAndroid_SetColorAdjust(SDL_Vout *vout, float brightness, float saturation, float contrast);

/* 无视频区域（黑边）的背景色，0~255，默认黑。 */
void SDL_VoutAndroid_SetBackgroundColor(SDL_Vout *vout, int red, int green, int blue);

/* HDR：直出由系统 display 负责，这里只记录状态供面板展示。 */
void SDL_VoutAndroid_SetAllowHDRDirectDisplay(SDL_Vout *vout, int allow);
int  SDL_VoutAndroid_IsDirectDisplayHDRSupported(SDL_Vout *vout);
int  SDL_VoutAndroid_IsHDRContent(SDL_Vout *vout);

/*
 * 取一张快照：软解路径返回最近一帧的 RGBA8888（malloc，调用方 free）；
 * 硬解直出路径当前帧不在 CPU 侧，返回 -1。
 */
int SDL_VoutAndroid_TakeSnapshot(SDL_Vout *vout, int type,
                                 int *out_w, int *out_h, void **out_pixels);

/*
 * 标记「硬解直出」通道：由 ffpipenode_android_mediacodec_direct 创建成功时置 1。
 * 用于诊断面板与内部路径判定。
 */
void SDL_VoutAndroid_SetDirectDecodeActive(SDL_Vout *vout, int active);

#endif
