/*****************************************************************************
 * ijksdl_gpu_stub.c
 *****************************************************************************
 *
 * Android 构建剩下的零碎桩函数。
 *
 * SDL_GPU（SDL_TextureOverlay / SDL_FBOOverlay / SDL_GPU）的真正实现是
 * vulkan/ijksdl_gpu_vulkan.c（apple 侧是 ijksdl_gpu_metal.m），
 * 引用计数与释放函数也在那里，这里只留保存图片与 gprof 的空实现。
 *****************************************************************************/

#include "ijksdl/ijksdl_gpu.h"
#include <stdlib.h>

void SaveIMGToFile(uint8_t *data, int width, int height, IMG_FORMAT format, char *tag, int pts)
{
    (void)data; (void)width; (void)height; (void)format; (void)tag; (void)pts;
}

// gprof 性能分析（ijkplayer-cmake 的 prof.c），安卓最小可播放不需要，空实现
void monstartup(const char *libname)
{
    (void)libname;
}

void moncleanup(void)
{
}

/*
 * 直出方案把 Vulkan 移出编译后，SDL_TextureOverlay / SDL_FBOOverlay 的引用计数
 * 实现（原本在 vulkan/ijksdl_gpu_vulkan.c）也一起没了。
 *
 * 字幕已改为「native 出 RGBA 位图 → Java 层绘制」，不再需要 GPU 纹理，
 * 因此这里保留空实现让旧代码路径可以链接通过（ff_sub_upload_texture 在
 * gpu == NULL 时会在使用纹理之前就返回 -1，不会真的走到这些对象上）。
 */
SDL_TextureOverlay *SDL_TextureOverlay_Retain(SDL_TextureOverlay *t)
{
    return t;
}

void SDL_TextureOverlay_Release(SDL_TextureOverlay **tp)
{
    if (tp)
        *tp = NULL;
}

void SDL_FBOOverlayFreeP(SDL_FBOOverlay **poverlay)
{
    if (poverlay)
        *poverlay = NULL;
}
