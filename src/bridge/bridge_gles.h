#ifndef BRIDGE_GLES_H
#define BRIDGE_GLES_H

#include <stdint.h>
#include "../elf32/elf32_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize OpenGL function pointers using SDL_GL_GetProcAddress */
int bridge_gles_init(void);

/* Check if symbol is a GLES function and return wrapper */
typedef uint32_t (*svc_handler_fn)(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp);
svc_handler_fn bridge_gles_lookup(const char *name);

/* Telemetry getters */
void bridge_gles_get_frame_stats(uint32_t *draw_arrays, uint32_t *draw_elements, uint32_t *clears, uint32_t *fbo);
void bridge_gles_reset_frame_stats(void);
uint32_t bridge_gles_get_bound_fbo(void);
void bridge_gles_save_screenshot(const char *filename, int w, int h);
void bridge_gles_set_frame_number(int frame);
void bridge_gles_set_viewport_scaling(int enabled, int render_w, int render_h,
                                      int dst_x, int dst_y, int dst_w, int dst_h);
void bridge_gles_render_letterbox_bars(int window_w, int window_h);
void bridge_gles_set_debug_mode(int enabled);
/* Offscreen (optionally multisampled) framebuffer for drivers whose window framebuffer lacks MSAA / VSync.
 * init returns the sample count in use, or -1 if unavailable (window framebuffer is used directly). */
int  bridge_gles_offscreen_init(int render_w, int render_h, int samples);
int  bridge_gles_offscreen_active(void);
void bridge_gles_present(int window_w, int window_h);
void bridge_gles_begin_frame(void);
extern float g_max_anisotropy;

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_GLES_H */
