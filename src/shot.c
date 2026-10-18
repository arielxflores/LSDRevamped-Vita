/* Screenshots of the finished frame, as PNG (stb_image_write, public domain / MIT). */
#include "common.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_UNUSED
#include "stb_image_write.h"

void shot_write(const char *name, int w, int h)
{
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!px)
        return;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* GL rows are bottom-up: flip, and drop alpha */
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) {
        free(px);
        return;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t)(h - 1 - y) * w * 4;
        uint8_t *dst = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    free(px);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", plat_shot_dir(), name);
    double t0 = plat_seconds();
    int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    lsd_log("shot %s %s (%.0f ms)", path, ok ? "saved" : "FAILED", (plat_seconds() - t0) * 1000.0);
}
