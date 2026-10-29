#include "bridge_gles.h"
#include "soft_mods.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <GL/gl.h>

/* OpenGL ES 2.0 types */
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_ETC1_RGB8_OES 0x8D64
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER_BINDING 0x8CA7

/* Function pointers */
#define GL_FN(ret, name, args) typedef ret (APIENTRY *PFN_##name) args; static PFN_##name pfn_##name = NULL;

GL_FN(void, glActiveTexture, (GLenum texture))
GL_FN(void, glAttachShader, (GLuint program, GLuint shader))
GL_FN(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name))
GL_FN(void, glBindBuffer, (GLenum target, GLuint buffer))
GL_FN(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))
GL_FN(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer))
GL_FN(void, glBindTexture, (GLenum target, GLuint texture))
GL_FN(void, glBlendFunc, (GLenum sfactor, GLenum dfactor))
GL_FN(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))
GL_FN(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data))
GL_FN(GLenum, glCheckFramebufferStatus, (GLenum target))
GL_FN(void, glClear, (GLbitfield mask))
GL_FN(void, glClearColor, (GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha))
GL_FN(void, glClearDepthf, (GLclampf depth))
GL_FN(void, glClearStencil, (GLint s))
GL_FN(void, glColorMask, (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha))
GL_FN(void, glCompileShader, (GLuint shader))
GL_FN(void, glCompressedTexImage2D, (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data))
GL_FN(void, glCopyTexImage2D, (GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border))
GL_FN(void, glCopyTexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height))
GL_FN(GLuint, glCreateProgram, (void))
GL_FN(GLuint, glCreateShader, (GLenum type))
GL_FN(void, glCullFace, (GLenum mode))
GL_FN(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers))
GL_FN(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))
GL_FN(void, glDeleteProgram, (GLuint program))
GL_FN(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers))
GL_FN(void, glDeleteShader, (GLuint shader))
GL_FN(void, glDeleteTextures, (GLsizei n, const GLuint *textures))
GL_FN(void, glDepthFunc, (GLenum func))
GL_FN(void, glDepthMask, (GLboolean flag))
GL_FN(void, glDisable, (GLenum cap))
GL_FN(void, glDisableVertexAttribArray, (GLuint index))
GL_FN(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))
GL_FN(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices))
GL_FN(void, glEnable, (GLenum cap))
GL_FN(void, glEnableVertexAttribArray, (GLuint index))
GL_FN(void, glFinish, (void))
GL_FN(void, glFlush, (void))
GL_FN(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer))
GL_FN(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))
GL_FN(void, glGenBuffers, (GLsizei n, GLuint *buffers))
GL_FN(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers))
GL_FN(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers))
GL_FN(void, glGenTextures, (GLsizei n, GLuint *textures))
GL_FN(void, glGetBufferParameteriv, (GLenum target, GLenum pname, GLint *params))
GL_FN(GLenum, glGetError, (void))
GL_FN(void, glGetFloatv, (GLenum pname, GLfloat *params))
GL_FN(void, glGetIntegerv, (GLenum pname, GLint *params))
GL_FN(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))
GL_FN(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params))
GL_FN(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))
GL_FN(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params))
GL_FN(const GLubyte *, glGetString, (GLenum name))
GL_FN(GLint, glGetUniformLocation, (GLuint program, const GLchar *name))
GL_FN(void, glLinkProgram, (GLuint program))

/* Telemetry counters */
static uint32_t g_draw_arrays_calls = 0;
static uint32_t g_draw_elements_calls = 0;
static uint32_t g_clear_calls = 0;
static uint32_t g_bound_fbo = 0;

static uint32_t g_tex_compressed_calls = 0;
GL_FN(void, glPixelStorei, (GLenum pname, GLint param))
GL_FN(void, glPolygonOffset, (GLfloat factor, GLfloat units))
GL_FN(void, glReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels))
GL_FN(void, glRenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height))
GL_FN(void, glRenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height))
GL_FN(void, glBlitFramebuffer, (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter))
GL_FN(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height))
GL_FN(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length))
GL_FN(void, glStencilFunc, (GLenum func, GLint ref, GLuint mask))
GL_FN(void, glStencilMask, (GLuint mask))
GL_FN(void, glStencilOp, (GLenum fail, GLenum zfail, GLenum zpass))
GL_FN(void, glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels))
GL_FN(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))
GL_FN(void, glTexParameterf, (GLenum target, GLenum pname, GLfloat param))
GL_FN(void, glTexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels))
GL_FN(void, glUniform1i, (GLint location, GLint v0))
GL_FN(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))
GL_FN(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat *value))
GL_FN(void, glUseProgram, (GLuint program))
GL_FN(void, glValidateProgram, (GLuint program))
GL_FN(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))
GL_FN(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height))

#define LOAD_PROC(name) pfn_##name = (PFN_##name)SDL_GL_GetProcAddress(#name);

static int s_gles_debug_mode = 0;
void bridge_gles_set_debug_mode(int enabled) {
    s_gles_debug_mode = enabled;
}

int bridge_gles_init(void) {
    LOAD_PROC(glActiveTexture)
    LOAD_PROC(glAttachShader)
    LOAD_PROC(glBindAttribLocation)
    LOAD_PROC(glBindBuffer)
    LOAD_PROC(glBindFramebuffer)
    LOAD_PROC(glBindRenderbuffer)
    LOAD_PROC(glBindTexture)
    LOAD_PROC(glBlendFunc)
    LOAD_PROC(glBufferData)
    LOAD_PROC(glBufferSubData)
    LOAD_PROC(glCheckFramebufferStatus)
    LOAD_PROC(glClear)
    LOAD_PROC(glClearColor)
    LOAD_PROC(glClearDepthf)
    LOAD_PROC(glClearStencil)
    LOAD_PROC(glColorMask)
    LOAD_PROC(glCompileShader)
    LOAD_PROC(glCompressedTexImage2D)
    LOAD_PROC(glCopyTexImage2D)
    LOAD_PROC(glCopyTexSubImage2D)
    LOAD_PROC(glCreateProgram)
    LOAD_PROC(glCreateShader)
    LOAD_PROC(glCullFace)
    LOAD_PROC(glDeleteBuffers)
    LOAD_PROC(glDeleteFramebuffers)
    LOAD_PROC(glDeleteProgram)
    LOAD_PROC(glDeleteRenderbuffers)
    LOAD_PROC(glDeleteShader)
    LOAD_PROC(glDeleteTextures)
    LOAD_PROC(glDepthFunc)
    LOAD_PROC(glDepthMask)
    LOAD_PROC(glDisable)
    LOAD_PROC(glDisableVertexAttribArray)
    LOAD_PROC(glDrawArrays)
    LOAD_PROC(glDrawElements)
    LOAD_PROC(glEnable)
    LOAD_PROC(glEnableVertexAttribArray)
    LOAD_PROC(glFinish)
    LOAD_PROC(glFlush)
    LOAD_PROC(glFramebufferRenderbuffer)
    LOAD_PROC(glFramebufferTexture2D)
    LOAD_PROC(glGenBuffers)
    LOAD_PROC(glGenFramebuffers)
    LOAD_PROC(glGenRenderbuffers)
    LOAD_PROC(glGenTextures)
    LOAD_PROC(glGetBufferParameteriv)
    LOAD_PROC(glGetError)
    LOAD_PROC(glGetFloatv)
    LOAD_PROC(glGetIntegerv)
    LOAD_PROC(glGetProgramInfoLog)
    LOAD_PROC(glGetProgramiv)
    LOAD_PROC(glGetShaderInfoLog)
    LOAD_PROC(glGetShaderiv)
    LOAD_PROC(glGetString)
    LOAD_PROC(glGetUniformLocation)
    LOAD_PROC(glLinkProgram)
    LOAD_PROC(glPixelStorei)
    LOAD_PROC(glPolygonOffset)
    LOAD_PROC(glReadPixels)
    LOAD_PROC(glRenderbufferStorage)
    LOAD_PROC(glRenderbufferStorageMultisample)
    LOAD_PROC(glBlitFramebuffer)
    LOAD_PROC(glScissor)
    LOAD_PROC(glShaderSource)
    LOAD_PROC(glStencilFunc)
    LOAD_PROC(glStencilMask)
    LOAD_PROC(glStencilOp)
    LOAD_PROC(glTexImage2D)
    LOAD_PROC(glTexParameteri)
    LOAD_PROC(glTexParameterf)
    LOAD_PROC(glTexSubImage2D)
    LOAD_PROC(glUniform1i)
    LOAD_PROC(glUniform4f)
    LOAD_PROC(glUniform4fv)
    LOAD_PROC(glUseProgram)
    LOAD_PROC(glValidateProgram)
    LOAD_PROC(glVertexAttribPointer)
    LOAD_PROC(glViewport)

    if (pfn_glEnable) {
        pfn_glEnable(0x809D /* GL_MULTISAMPLE */);
    }

    if (s_gles_debug_mode) {
        printf("[+] bridge_gles: OpenGL function pointers initialized (8x MSAA + AF enabled)\n");
    }
    return 1;
}

/* Offscreen (multisampled) default framebuffer.
 * Some drivers (Optimus/hybrid dGPUs) hand out a window framebuffer with no MSAA and an ignored swap
 * interval. When enabled, the guest's "framebuffer 0" is redirected to an FBO at render resolution and
 * bridge_gles_present() resolves/scales it to the window in one short blit right before the swap. */
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_READ_FRAMEBUFFER_ 0x8CA8
#define GL_DRAW_FRAMEBUFFER_ 0x8CA9
#define GL_RENDERBUFFER_ 0x8D41
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT_ 0x821A
#define GL_DEPTH24_STENCIL8_ 0x88F0
#define GL_RGBA8_ 0x8058
#define GL_MAX_SAMPLES_ 0x8D57
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
static struct {
    int      active;
    int      samples;
    int      w, h;
    GLuint   main_fbo, main_color_rb, main_depth_rb;   /* what the guest renders into */
    GLuint   resolve_fbo, resolve_color_rb;            /* single-sample copy used for scaling/readback */
} s_off;

/* Framebuffer that holds the finished (single-sample) image */
static GLuint off_display_fbo(void) {
    return (s_off.samples > 0) ? s_off.resolve_fbo : s_off.main_fbo;
}

/* Resolve the multisampled frame into the single-sample FBO; leaves the guest's framebuffer bound */
static void off_resolve(void) {
    if (!s_off.active || s_off.samples <= 0 || !pfn_glBlitFramebuffer) return;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    pfn_glBindFramebuffer(GL_READ_FRAMEBUFFER_, s_off.main_fbo);
    pfn_glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, s_off.resolve_fbo);
    pfn_glBlitFramebuffer(0, 0, s_off.w, s_off.h, 0, 0, s_off.w, s_off.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    pfn_glBindFramebuffer(GL_FRAMEBUFFER_, g_bound_fbo ? g_bound_fbo : s_off.main_fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

static void off_destroy(void) {
    if (pfn_glBindFramebuffer) pfn_glBindFramebuffer(GL_FRAMEBUFFER_, 0);
    if (s_off.main_fbo && pfn_glDeleteFramebuffers) pfn_glDeleteFramebuffers(1, &s_off.main_fbo);
    if (s_off.resolve_fbo && pfn_glDeleteFramebuffers) pfn_glDeleteFramebuffers(1, &s_off.resolve_fbo);
    if (s_off.main_color_rb && pfn_glDeleteRenderbuffers) pfn_glDeleteRenderbuffers(1, &s_off.main_color_rb);
    if (s_off.main_depth_rb && pfn_glDeleteRenderbuffers) pfn_glDeleteRenderbuffers(1, &s_off.main_depth_rb);
    if (s_off.resolve_color_rb && pfn_glDeleteRenderbuffers) pfn_glDeleteRenderbuffers(1, &s_off.resolve_color_rb);
    memset(&s_off, 0, sizeof(s_off));
}

/* Creates the offscreen framebuffers and redirects framebuffer 0 to them. Returns the sample count in use
 * (0 = no MSAA) or -1 if the offscreen path could not be set up (the window framebuffer is then used as-is). */
int bridge_gles_offscreen_init(int w, int h, int samples) {
    if (!pfn_glGenFramebuffers || !pfn_glGenRenderbuffers || !pfn_glBindRenderbuffer || !pfn_glFramebufferRenderbuffer ||
        !pfn_glCheckFramebufferStatus || !pfn_glBlitFramebuffer || !pfn_glBindFramebuffer) {
        return -1;
    }
    GLint max_samples = 0;
    if (pfn_glGetIntegerv) pfn_glGetIntegerv(GL_MAX_SAMPLES_, &max_samples);
    if (samples > max_samples) samples = max_samples;
    if (samples < 2 || !pfn_glRenderbufferStorageMultisample) samples = 0;

    for (;;) {
        memset(&s_off, 0, sizeof(s_off));
        s_off.w = w; s_off.h = h; s_off.samples = samples;

        pfn_glGenFramebuffers(1, &s_off.main_fbo);
        pfn_glGenRenderbuffers(1, &s_off.main_color_rb);
        pfn_glGenRenderbuffers(1, &s_off.main_depth_rb);
        pfn_glBindRenderbuffer(GL_RENDERBUFFER_, s_off.main_color_rb);
        if (samples > 0) pfn_glRenderbufferStorageMultisample(GL_RENDERBUFFER_, samples, GL_RGBA8_, w, h);
        else             pfn_glRenderbufferStorage(GL_RENDERBUFFER_, GL_RGBA8_, w, h);
        pfn_glBindRenderbuffer(GL_RENDERBUFFER_, s_off.main_depth_rb);
        if (samples > 0) pfn_glRenderbufferStorageMultisample(GL_RENDERBUFFER_, samples, GL_DEPTH24_STENCIL8_, w, h);
        else             pfn_glRenderbufferStorage(GL_RENDERBUFFER_, GL_DEPTH24_STENCIL8_, w, h);
        pfn_glBindFramebuffer(GL_FRAMEBUFFER_, s_off.main_fbo);
        pfn_glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_RENDERBUFFER_, s_off.main_color_rb);
        pfn_glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, GL_RENDERBUFFER_, s_off.main_depth_rb);
        int ok = (pfn_glCheckFramebufferStatus(GL_FRAMEBUFFER_) == GL_FRAMEBUFFER_COMPLETE_);

        if (ok && samples > 0) {
            pfn_glGenFramebuffers(1, &s_off.resolve_fbo);
            pfn_glGenRenderbuffers(1, &s_off.resolve_color_rb);
            pfn_glBindRenderbuffer(GL_RENDERBUFFER_, s_off.resolve_color_rb);
            pfn_glRenderbufferStorage(GL_RENDERBUFFER_, GL_RGBA8_, w, h);
            pfn_glBindFramebuffer(GL_FRAMEBUFFER_, s_off.resolve_fbo);
            pfn_glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_RENDERBUFFER_, s_off.resolve_color_rb);
            ok = (pfn_glCheckFramebufferStatus(GL_FRAMEBUFFER_) == GL_FRAMEBUFFER_COMPLETE_);
        }
        if (ok) break;

        off_destroy();
        if (samples == 0) return -1;
        samples = (samples > 2) ? samples / 2 : 0;   /* step down 8 -> 4 -> 2 -> off */
    }

    s_off.active = 1;
    pfn_glBindFramebuffer(GL_FRAMEBUFFER_, s_off.main_fbo);
    return s_off.samples;
}

int bridge_gles_offscreen_active(void) {
    return s_off.active;
}

#define G_PTR(addr) ((addr) ? (img->mem + (addr)) : NULL)
#define READ_STACK(idx) (*(uint32_t*)(img->mem + sp + (idx) * 4))

static uint32_t wrap_glActiveTexture(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glActiveTexture) pfn_glActiveTexture(r0);
    return 0;
}
static uint32_t wrap_glAttachShader(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glAttachShader) pfn_glAttachShader(r0, r1);
    return 0;
}
static uint32_t wrap_glBindAttribLocation(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glBindAttribLocation) pfn_glBindAttribLocation(r0, r1, (const GLchar*)G_PTR(r2));
    return 0;
}
static uint32_t wrap_glBindBuffer(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glBindBuffer) pfn_glBindBuffer(r0, r1);
    return 0;
}
static uint32_t wrap_glBindFramebuffer(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    g_bound_fbo = r1;
    if (pfn_glBindFramebuffer) pfn_glBindFramebuffer(r0, (r1 == 0 && s_off.active) ? s_off.main_fbo : r1);
    return 0;
}
static uint32_t wrap_glBindRenderbuffer(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glBindRenderbuffer) pfn_glBindRenderbuffer(r0, r1);
    return 0;
}
static uint32_t wrap_glBindTexture(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glBindTexture) pfn_glBindTexture(r0, r1);
    return 0;
}
static uint32_t wrap_glBlendFunc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glBlendFunc) pfn_glBlendFunc(r0, r1);
    return 0;
}
static uint32_t wrap_glBufferData(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glBufferData) pfn_glBufferData(r0, r1, G_PTR(r2), r3);
    return 0;
}
static uint32_t wrap_glBufferSubData(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glBufferSubData) pfn_glBufferSubData(r0, r1, r2, G_PTR(r3));
    return 0;
}
static uint32_t wrap_glCheckFramebufferStatus(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return pfn_glCheckFramebufferStatus ? pfn_glCheckFramebufferStatus(r0) : 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */;
}
static uint32_t wrap_glClear(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    g_clear_calls++;
    if (pfn_glClear) pfn_glClear(r0);
    return 0;
}
static uint32_t wrap_glClearColor(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float c[4];
    memcpy(&c[0], &r0, 4); memcpy(&c[1], &r1, 4); memcpy(&c[2], &r2, 4); memcpy(&c[3], &r3, 4);
    if (s_gles_debug_mode) {
        static int s_cc_logged = 0;
        if (s_cc_logged++ < 5) {
            printf("[GL] glClearColor(%.3f, %.3f, %.3f, %.3f)\n", c[0], c[1], c[2], c[3]);
        }
    }
    if (pfn_glClearColor) pfn_glClearColor(c[0], c[1], c[2], c[3]);
    return 0;
}
static uint32_t wrap_glClearDepthf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float d;
    memcpy(&d, &r0, 4);
    if (pfn_glClearDepthf) pfn_glClearDepthf(d);
    return 0;
}
static uint32_t wrap_glClearStencil(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glClearStencil) pfn_glClearStencil(r0);
    return 0;
}
static uint32_t wrap_glColorMask(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glColorMask) pfn_glColorMask(r0, r1, r2, r3);
    return 0;
}
static uint32_t wrap_glCompileShader(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glCompileShader) {
        pfn_glCompileShader(r0);
        if (pfn_glGetShaderiv) {
            GLint compiled = 0;
            pfn_glGetShaderiv(r0, 0x8B81 /* GL_COMPILE_STATUS */, &compiled);
            if (!compiled && pfn_glGetShaderInfoLog) {
                char log[1024];
                GLsizei len = 0;
                pfn_glGetShaderInfoLog(r0, sizeof(log), &len, log);
                fprintf(stderr, "[-] Shader %u compile FAILED: %s\n", r0, log);
            }
        }
    }
    return 0;
}
/* Standard ETC1 block decompressor (converts 4x4 ETC1 compressed block to 24-bit RGB) */
static const int s_etc1_modifiers[8][4] = {
    { 2,   8,  -2,   -8 },
    { 5,  17,  -5,  -17 },
    { 9,  29,  -9,  -29 },
    { 13,  42, -13,  -42 },
    { 18,  60, -18,  -60 },
    { 24,  80, -24,  -80 },
    { 33, 106, -33, -106 },
    { 47, 183, -47, -183 }
};

static inline uint8_t etc1_clamp(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

static inline uint8_t convert4To8(int b) {
    b &= 0xF;
    return (uint8_t)((b << 4) | b);
}

static inline uint8_t convert5To8(int b) {
    b &= 0x1F;
    return (uint8_t)((b << 3) | (b >> 2));
}

static inline uint8_t convertDiff(int base, int diff) {
    diff &= 7;
    if (diff & 4) diff -= 8;
    return convert5To8(base + diff);
}

static void etc1_decode_subblock(uint8_t *dst_rgb, int stride_rgb, int r, int g, int b,
                                 const int *table, uint32_t low, int second, int flipped,
                                 int max_w, int max_h) {
    int baseX = (second && !flipped) ? 2 : 0;
    int baseY = (second && flipped) ? 2 : 0;

    for (int i = 0; i < 8; ++i) {
        int x, y;
        if (flipped) {
            x = baseX + (i >> 1);
            y = baseY + (i & 1);
        } else {
            x = baseX + (i >> 2);
            y = baseY + (i & 3);
        }
        if (x >= max_w || y >= max_h) continue;

        int k = y + (x * 4);
        int offset = ((low >> k) & 1) | ((low >> (k + 15)) & 2);
        int delta = table[offset];

        uint8_t *q = dst_rgb + y * stride_rgb + x * 3;
        q[0] = etc1_clamp(r + delta);
        q[1] = etc1_clamp(g + delta);
        q[2] = etc1_clamp(b + delta);
    }
}

static void etc1_decode_block(const uint8_t *src, uint8_t *dst_rgb, int stride_rgb, int max_w, int max_h) {
    uint32_t high = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
    uint32_t low  = ((uint32_t)src[4] << 24) | ((uint32_t)src[5] << 16) | ((uint32_t)src[6] << 8) | src[7];

    int r1, r2, g1, g2, b1, b2;
    if (high & 2) {
        int rBase = (high >> 27) & 0x1F;
        int gBase = (high >> 19) & 0x1F;
        int bBase = (high >> 11) & 0x1F;
        r1 = convert5To8(rBase);
        r2 = convertDiff(rBase, high >> 24);
        g1 = convert5To8(gBase);
        g2 = convertDiff(gBase, high >> 16);
        b1 = convert5To8(bBase);
        b2 = convertDiff(bBase, high >> 8);
    } else {
        r1 = convert4To8(high >> 28);
        r2 = convert4To8(high >> 24);
        g1 = convert4To8(high >> 20);
        g2 = convert4To8(high >> 16);
        b1 = convert4To8(high >> 12);
        b2 = convert4To8(high >> 8);
    }

    int tableIndexA = 7 & (high >> 5);
    int tableIndexB = 7 & (high >> 2);
    const int *tableA = s_etc1_modifiers[tableIndexA];
    const int *tableB = s_etc1_modifiers[tableIndexB];
    int flipped = (high & 1) != 0;

    etc1_decode_subblock(dst_rgb, stride_rgb, r1, g1, b1, tableA, low, 0, flipped, max_w, max_h);
    etc1_decode_subblock(dst_rgb, stride_rgb, r2, g2, b2, tableB, low, 1, flipped, max_w, max_h);
}

static uint8_t *etc1_decode_image(const uint8_t *src, int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    uint8_t *dst = (uint8_t*)calloc(1, width * height * 3);
    if (!dst) return NULL;
    int stride = width * 3;
    const uint8_t *block_ptr = src;
    for (int by = 0; by < height; by += 4) {
        int max_h = (height - by < 4) ? (height - by) : 4;
        for (int bx = 0; bx < width; bx += 4) {
            int max_w = (width - bx < 4) ? (width - bx) : 4;
            etc1_decode_block(block_ptr, dst + by * stride + bx * 3, stride, max_w, max_h);
            block_ptr += 8;
        }
    }
    return dst;
}

static uint32_t wrap_glCompressedTexImage2D(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLenum target = r0;
    GLint level = r1;
    GLenum internalformat = r2;
    GLsizei w = r3;
    GLsizei h = READ_STACK(0);
    GLint border = READ_STACK(1);
    GLsizei imageSize = READ_STACK(2);
    const void *data = G_PTR(READ_STACK(3));

    g_tex_compressed_calls++;

    /* Software ETC1 decompression fallback for Desktop OpenGL */
    if (internalformat == 0x8D64 /* GL_ETC1_RGB8_OES */) {
        if (s_gles_debug_mode) {
            static int s_etc1_logged = 0;
            if (s_etc1_logged++ < 5) {
                fprintf(stderr, "[GL] Decoding ETC1 texture in software: %dx%d (%d bytes)\n", w, h, imageSize);
            }
        }
        if (data && w > 0 && h > 0) {
            uint8_t *rgb = etc1_decode_image((const uint8_t*)data, w, h);
            if (rgb) {
                if (pfn_glTexImage2D) {
                    pfn_glTexImage2D(target, level, 0x1907 /* GL_RGB */, w, h, border, 0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, rgb);
                }
                free(rgb);
                return 0;
            }
        }
    }

    soft_mods_patch_compressed_texture((int)level, (int)w, (int)h, (int)imageSize, (void*)data);

    if (pfn_glCompressedTexImage2D) pfn_glCompressedTexImage2D(target, level, internalformat, w, h, border, imageSize, data);
    return 0;
}
static uint32_t wrap_glCopyTexImage2D(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLint y = (GLint)READ_STACK(0);
    GLsizei w = (GLsizei)READ_STACK(1);
    GLsizei h = (GLsizei)READ_STACK(2);
    GLint border = (GLint)READ_STACK(3);
    if (pfn_glCopyTexImage2D) pfn_glCopyTexImage2D(r0, r1, r2, (GLint)r3, y, w, h, border);
    return 0;
}
static uint32_t wrap_glCopyTexSubImage2D(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLint x = (GLint)READ_STACK(0);
    GLint y = (GLint)READ_STACK(1);
    GLsizei w = (GLsizei)READ_STACK(2);
    GLsizei h = (GLsizei)READ_STACK(3);
    if (pfn_glCopyTexSubImage2D) pfn_glCopyTexSubImage2D(r0, r1, r2, r3, x, y, w, h);
    return 0;
}
static uint32_t wrap_glCreateProgram(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return pfn_glCreateProgram ? pfn_glCreateProgram() : 0;
}
static uint32_t wrap_glCreateShader(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return pfn_glCreateShader ? pfn_glCreateShader(r0) : 0;
}
static uint32_t wrap_glCullFace(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glCullFace) pfn_glCullFace(r0);
    return 0;
}
static uint32_t wrap_glDeleteBuffers(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDeleteBuffers) pfn_glDeleteBuffers(r0, (const GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glDeleteFramebuffers(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDeleteFramebuffers) pfn_glDeleteFramebuffers(r0, (const GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glDeleteProgram(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDeleteProgram) pfn_glDeleteProgram(r0);
    return 0;
}
static uint32_t wrap_glDeleteRenderbuffers(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDeleteRenderbuffers) pfn_glDeleteRenderbuffers(r0, (const GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glDeleteShader(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDeleteShader) pfn_glDeleteShader(r0);
    return 0;
}
static uint32_t wrap_glDeleteTextures(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDeleteTextures) pfn_glDeleteTextures(r0, (const GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glDepthFunc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDepthFunc) pfn_glDepthFunc(r0);
    return 0;
}
static uint32_t wrap_glDepthMask(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDepthMask) pfn_glDepthMask(r0);
    return 0;
}
static int s_blend_enabled = 0;
static uint32_t wrap_glDisable(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 == 0x0BE2 /* GL_BLEND */) s_blend_enabled = 0;
    if (pfn_glDisable) pfn_glDisable(r0);
    return 0;
}
static uint32_t wrap_glDisableVertexAttribArray(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glDisableVertexAttribArray) pfn_glDisableVertexAttribArray(r0);
    return 0;
}
static int s_current_frame = 0;
void bridge_gles_set_frame_number(int frame) {
    s_current_frame = frame;
}

static uint32_t wrap_glDrawArrays(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    g_draw_arrays_calls++;
    if (s_gles_debug_mode && s_current_frame == 220) {
        GLint prog = 0, fbo = 0, tex = 0, vbo = 0;
        if (pfn_glGetIntegerv) {
            pfn_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
            pfn_glGetIntegerv(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &fbo);
            pfn_glGetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &tex);
            pfn_glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &vbo);
        }
        printf("[DRAW_AR 220] mode=0x%X first=%u cnt=%u | prog=%d fbo=%d tex=%d blend=%d vbo=%d\n",
               r0, r1, r2, prog, fbo, tex, s_blend_enabled, vbo);
    }
    if (pfn_glDrawArrays) pfn_glDrawArrays(r0, r1, r2);
    return 0;
}
static uint32_t wrap_glDrawElements(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    g_draw_elements_calls++;
    if (s_gles_debug_mode && s_current_frame == 220) {
        GLint prog = 0, fbo = 0, tex = 0, vbo = 0, ibo = 0;
        if (pfn_glGetIntegerv) {
            pfn_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
            pfn_glGetIntegerv(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &fbo);
            pfn_glGetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &tex);
            pfn_glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &vbo);
            pfn_glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &ibo);
        }
        printf("[DRAW_EL 220] mode=0x%X cnt=%u type=0x%X | prog=%d fbo=%d tex=%d blend=%d vbo=%d ibo=%d\n",
               r0, r1, r2, prog, fbo, tex, s_blend_enabled, vbo, ibo);
    }
    /* Workaround for untextured preview placeholder in the in-game store:
     * When inspecting store props (e.g. store_props_preview_background_Plane002), the original
     * Android engine submits an untextured 12-index quad without blending enabled if the dynamic
     * render-to-texture preview has not finished loading or fails to bind. Drawing it would obscure
     * the store chamber with an opaque solid quad. Dropping this specific draw call allows the
     * backdrop wallpaper to remain cleanly visible. */
    if (r1 == 12 && !s_blend_enabled) {
        GLint fbo = 0, tex = 0;
        if (pfn_glGetIntegerv) {
            pfn_glGetIntegerv(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &fbo);
            pfn_glGetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &tex);
        }
        if (fbo == 0 && tex == 0) {
            return 0;
        }
    }
    GLint bound_ibo = 0;
    if (pfn_glGetIntegerv) pfn_glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &bound_ibo);
    const void *indices = bound_ibo ? (const void*)(uintptr_t)r3 : G_PTR(r3);
    if (pfn_glDrawElements) pfn_glDrawElements(r0, r1, r2, indices);
    return 0;
}
static uint32_t wrap_glEnable(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (r0 == 0x0BE2 /* GL_BLEND */) s_blend_enabled = 1;
    if (pfn_glEnable) pfn_glEnable(r0);
    return 0;
}
static uint32_t wrap_glEnableVertexAttribArray(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glEnableVertexAttribArray) pfn_glEnableVertexAttribArray(r0);
    return 0;
}
static uint32_t wrap_glFinish(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glFinish) pfn_glFinish();
    return 0;
}
static uint32_t wrap_glFlush(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glFlush) pfn_glFlush();
    return 0;
}
static uint32_t wrap_glFramebufferRenderbuffer(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glFramebufferRenderbuffer) pfn_glFramebufferRenderbuffer(r0, r1, r2, r3);
    return 0;
}
static uint32_t wrap_glFramebufferTexture2D(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLint level = READ_STACK(0);
    if (pfn_glFramebufferTexture2D) pfn_glFramebufferTexture2D(r0, r1, r2, r3, level);
    return 0;
}
static uint32_t wrap_glGenBuffers(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGenBuffers) pfn_glGenBuffers(r0, (GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glGenFramebuffers(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGenFramebuffers) pfn_glGenFramebuffers(r0, (GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glGenRenderbuffers(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGenRenderbuffers) pfn_glGenRenderbuffers(r0, (GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glGenTextures(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGenTextures) pfn_glGenTextures(r0, (GLuint*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glGetIntegerv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGetIntegerv) pfn_glGetIntegerv(r0, (GLint*)G_PTR(r1));
    if (s_off.active && (r0 == 0x8CA6 || r0 == 0x8CAA /* draw/read framebuffer binding */) && r1) {
        GLint *out = (GLint*)G_PTR(r1);
        if ((GLuint)*out == s_off.main_fbo) *out = 0;   /* the guest must keep seeing the window as framebuffer 0 */
    }
    return 0;
}
static uint32_t wrap_glGetProgramInfoLog(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGetProgramInfoLog) pfn_glGetProgramInfoLog(r0, r1, (GLsizei*)G_PTR(r2), (GLchar*)G_PTR(r3));
    return 0;
}
static uint32_t wrap_glGetProgramiv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGetProgramiv) pfn_glGetProgramiv(r0, r1, (GLint*)G_PTR(r2));
    return 0;
}
static uint32_t wrap_glGetShaderInfoLog(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGetShaderInfoLog) pfn_glGetShaderInfoLog(r0, r1, (GLsizei*)G_PTR(r2), (GLchar*)G_PTR(r3));
    return 0;
}
static uint32_t wrap_glGetShaderiv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGetShaderiv) pfn_glGetShaderiv(r0, r1, (GLint*)G_PTR(r2));
    return 0;
}
static uint32_t wrap_glGetBufferParameteriv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGetBufferParameteriv && r2) pfn_glGetBufferParameteriv(r0, r1, (GLint*)G_PTR(r2));
    return 0;
}
static uint32_t wrap_glGetFloatv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glGetFloatv) pfn_glGetFloatv(r0, (GLfloat*)G_PTR(r1));
    return 0;
}
static uint32_t wrap_glGetString(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    const char *str = (const char*)(pfn_glGetString ? pfn_glGetString(r0) : NULL);
    uint32_t slot_addr = 0;
    const char *fallback = "";
    if (r0 == 0x1F00 /* GL_VENDOR */) {
        slot_addr = 0x00038000u;
        fallback = "NVIDIA Corporation";
    } else if (r0 == 0x1F01 /* GL_RENDERER */) {
        slot_addr = 0x00038200u;
        fallback = "GeForce GTX";
    } else if (r0 == 0x1F02 /* GL_VERSION */) {
        slot_addr = 0x00038400u;
        fallback = "OpenGL ES 2.0";
        /* Always report OpenGL ES 2.0 to guest */
        size_t len = strlen(fallback);
        memcpy(img->mem + slot_addr, fallback, len + 1);
        return slot_addr;
    } else if (r0 == 0x1F03 /* GL_EXTENSIONS */) {
        slot_addr = 0x00038800u;
        static char ext_buf[8192];
        snprintf(ext_buf, sizeof(ext_buf),
            "GL_OES_compressed_ETC1_RGB8_texture "
            "GL_OES_depth24 "
            "GL_OES_packed_depth_stencil "
            "GL_OES_depth_texture "
            "GL_OES_standard_derivatives "
            "GL_OES_rgb8_rgba8 "
            "GL_OES_texture_npot "
            "GL_OES_mapbuffer "
            "GL_EXT_texture_format_BGRA8888 "
            "GL_EXT_texture_filter_anisotropic "
            "GL_EXT_texture_compression_dxt1 "
            "GL_EXT_texture_compression_s3tc "
            "%s", str ? str : "");
        size_t len = strlen(ext_buf);
        if (len > 8000) len = 8000;
        memcpy(img->mem + slot_addr, ext_buf, len);
        img->mem[slot_addr + len] = '\0';
        return slot_addr;
    }
    if (!slot_addr) return 0;
    const char *chosen = str ? str : fallback;
    size_t len = strlen(chosen);
    if (len > 3000) len = 3000;
    memcpy(img->mem + slot_addr, chosen, len);
    img->mem[slot_addr + len] = '\0';
    return slot_addr;
}
static uint32_t wrap_glGetUniformLocation(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    return pfn_glGetUniformLocation ? pfn_glGetUniformLocation(r0, (const GLchar*)G_PTR(r1)) : -1;
}
static uint32_t wrap_glLinkProgram(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glLinkProgram) {
        pfn_glLinkProgram(r0);
        if (pfn_glGetProgramiv) {
            GLint linked = 0;
            pfn_glGetProgramiv(r0, 0x8B82 /* GL_LINK_STATUS */, &linked);
            if (!linked && pfn_glGetProgramInfoLog) {
                char log[1024];
                GLsizei len = 0;
                pfn_glGetProgramInfoLog(r0, sizeof(log), &len, log);
                fprintf(stderr, "[-] Program %u link FAILED: %s\n", r0, log);
            }
        }
    }
    return 0;
}
static uint32_t wrap_glPixelStorei(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glPixelStorei) pfn_glPixelStorei(r0, r1);
    return 0;
}
static uint32_t wrap_glPolygonOffset(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float f, u;
    memcpy(&f, &r0, 4); memcpy(&u, &r1, 4);
    if (pfn_glPolygonOffset) pfn_glPolygonOffset(f, u);
    return 0;
}
static uint32_t wrap_glReadPixels(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLsizei w = r2, h = r3;
    GLenum fmt = READ_STACK(0);
    GLenum type = READ_STACK(1);
    void *pixels = G_PTR(READ_STACK(2));
    if (s_off.active && g_bound_fbo == 0) {
        /* Reading the "window" while it is redirected to the offscreen FBO: read the resolved copy */
        off_resolve();
        pfn_glBindFramebuffer(GL_READ_FRAMEBUFFER_, off_display_fbo());
        if (pfn_glReadPixels) pfn_glReadPixels(r0, r1, w, h, fmt, type, pixels);
        pfn_glBindFramebuffer(GL_READ_FRAMEBUFFER_, s_off.main_fbo);
        return 0;
    }
    if (pfn_glReadPixels) pfn_glReadPixels(r0, r1, w, h, fmt, type, pixels);
    return 0;
}
static uint32_t wrap_glRenderbufferStorage(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glRenderbufferStorage) pfn_glRenderbufferStorage(r0, r1, r2, r3);
    return 0;
}
/* Viewport scaling state (for scaling lower render resolutions to fullscreen / window) */
static int   s_scale_enabled  = 0;
static int   s_render_w       = 1280;
static int   s_render_h       = 720;
static int   s_dst_x          = 0;
static int   s_dst_y          = 0;
static int   s_dst_w          = 1920;
static int   s_dst_h          = 1080;
static float s_scale_x        = 1.0f;
static float s_scale_y        = 1.0f;

void bridge_gles_set_viewport_scaling(int enabled, int render_w, int render_h,
                                      int dst_x, int dst_y, int dst_w, int dst_h) {
    s_scale_enabled = enabled;
    s_render_w      = (render_w > 0) ? render_w : 1280;
    s_render_h      = (render_h > 0) ? render_h : 720;
    s_dst_x         = dst_x;
    s_dst_y         = dst_y;
    s_dst_w         = dst_w;
    s_dst_h         = dst_h;
    if (enabled && s_render_w > 0 && s_render_h > 0) {
        s_scale_x = (float)dst_w / (float)s_render_w;
        s_scale_y = (float)dst_h / (float)s_render_h;
    } else {
        s_scale_x = 1.0f;
        s_scale_y = 1.0f;
    }
}

void bridge_gles_render_letterbox_bars(int window_w, int window_h) {
    if (!s_scale_enabled || (s_dst_x <= 0 && s_dst_y <= 0)) return;
    if (!pfn_glScissor || !pfn_glClear) return;

    glEnable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    if (s_dst_x > 0) {
        // Left pillarbox bar
        pfn_glScissor(0, 0, s_dst_x, window_h);
        pfn_glClear(GL_COLOR_BUFFER_BIT);
        // Right pillarbox bar
        pfn_glScissor(s_dst_x + s_dst_w, 0, window_w - (s_dst_x + s_dst_w), window_h);
        pfn_glClear(GL_COLOR_BUFFER_BIT);
    }
    if (s_dst_y > 0) {
        // Bottom letterbox bar
        pfn_glScissor(0, 0, window_w, s_dst_y);
        pfn_glClear(GL_COLOR_BUFFER_BIT);
        // Top letterbox bar
        pfn_glScissor(0, s_dst_y + s_dst_h, window_w, window_h - (s_dst_y + s_dst_h));
        pfn_glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

/* Copies the finished offscreen frame to the window (scaled / letterboxed). Call right before the swap. */
void bridge_gles_present(int window_w, int window_h) {
    if (!s_off.active) return;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);

    off_resolve();

    int dx = s_dst_x, dy = s_dst_y, dw = s_dst_w, dh = s_dst_h;
    if (!s_scale_enabled) { dx = 0; dy = 0; dw = window_w; dh = window_h; }

    pfn_glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    if (dx > 0 || dy > 0 || dw < window_w || dh < window_h) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);   /* letterbox / pillarbox bars */
    }
    pfn_glBindFramebuffer(GL_READ_FRAMEBUFFER_, off_display_fbo());
    pfn_glBlitFramebuffer(0, 0, s_off.w, s_off.h, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT,
                          (dw == s_off.w && dh == s_off.h) ? GL_NEAREST : GL_LINEAR);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

/* Re-binds the guest's framebuffer after the swap */
void bridge_gles_begin_frame(void) {
    if (!s_off.active) return;
    pfn_glBindFramebuffer(GL_FRAMEBUFFER_, g_bound_fbo ? g_bound_fbo : s_off.main_fbo);
}

static uint32_t wrap_glScissor(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLint x = (GLint)r0;
    GLint y = (GLint)r1;
    GLsizei w = (GLsizei)r2;
    GLsizei h = (GLsizei)r3;

    if (g_bound_fbo == 0 && s_scale_enabled && !s_off.active) {
        if (w > 0 && h > 0) {
            x = s_dst_x + (GLint)(x * s_scale_x + 0.5f);
            y = s_dst_y + (GLint)(y * s_scale_y + 0.5f);
            w = (GLsizei)(w * s_scale_x + 0.5f);
            h = (GLsizei)(h * s_scale_y + 0.5f);
        }
    }
    if (pfn_glScissor) pfn_glScissor(x, y, w, h);
    return 0;
}
static char *translate_gles_shader(const char *src, GLint len) {
    if (!src) return NULL;
    size_t in_len = (len > 0) ? (size_t)len : strlen(src);
    size_t out_cap = in_len + 512;
    char *out = (char*)malloc(out_cap);
    if (!out) return NULL;

    const char *header = 
        "#version 120\n"
        "#define lowp\n"
        "#define mediump\n"
        "#define highp\n";
    strcpy(out, header);
    size_t out_len = strlen(out);

#define APPEND_CHAR(ch) do { \
        if (out_len + 2 >= out_cap) { \
            out_cap = out_cap * 2 + 256; \
            char *new_out = (char*)realloc(out, out_cap); \
            if (!new_out) { free(out); return NULL; } \
            out = new_out; \
        } \
        out[out_len++] = (ch); \
    } while (0)

    const char *p = src;
    const char *end = src + in_len;

    while (p < end) {
        const char *line_start = p;
        while (p < end && (*p == ' ' || *p == '\t')) p++;

        if (p + 9 <= end && strncmp(p, "precision", 9) == 0 && (p + 9 == end || p[9] == ' ' || p[9] == '\t')) {
            APPEND_CHAR('/');
            APPEND_CHAR('/');
            while (p < end && *p != '\n') {
                APPEND_CHAR(*p++);
            }
            if (p < end && *p == '\n') {
                APPEND_CHAR(*p++);
            }
        } else {
            p = line_start;
            while (p < end && *p != '\n') {
                APPEND_CHAR(*p++);
            }
            if (p < end && *p == '\n') {
                APPEND_CHAR(*p++);
            }
        }
    }
#undef APPEND_CHAR
    out[out_len] = '\0';
    return out;
}

static uint32_t wrap_glShaderSource(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLsizei count = r1;
    const uint32_t *guest_ptrs = (const uint32_t*)G_PTR(r2);
    const GLint *lengths = (const GLint*)G_PTR(r3);
    if (!guest_ptrs || count <= 0) return 0;

    size_t total_len = 0;
    for (GLsizei i = 0; i < count; ++i) {
        const char *s = (const char*)G_PTR(guest_ptrs[i]);
        if (s) {
            GLint len = lengths ? lengths[i] : -1;
            total_len += (len >= 0) ? (size_t)len : strlen(s);
        }
    }

    char *combined = (char*)malloc(total_len + 1);
    if (!combined) return 0;
    size_t offset = 0;
    for (GLsizei i = 0; i < count; ++i) {
        const char *s = (const char*)G_PTR(guest_ptrs[i]);
        if (s) {
            GLint len = lengths ? lengths[i] : -1;
            size_t slen = (len >= 0) ? (size_t)len : strlen(s);
            memcpy(combined + offset, s, slen);
            offset += slen;
        }
    }
    combined[offset] = '\0';

    char *translated = translate_gles_shader(combined, (GLint)offset);
    free(combined);

    if (translated) {
        const char *src_ptr = translated;
        GLint trans_len = (GLint)strlen(translated);
        if (pfn_glShaderSource) pfn_glShaderSource(r0, 1, &src_ptr, &trans_len);
        free(translated);
    }
    return 0;
}
static uint32_t wrap_glStencilFunc(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glStencilFunc) pfn_glStencilFunc(r0, r1, r2);
    return 0;
}
static uint32_t wrap_glStencilMask(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glStencilMask) pfn_glStencilMask(r0);
    return 0;
}
static uint32_t wrap_glStencilOp(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glStencilOp) pfn_glStencilOp(r0, r1, r2);
    return 0;
}
static uint32_t wrap_glTexImage2D(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLsizei w = r3;
    GLsizei h = READ_STACK(0);
    GLint border = READ_STACK(1);
    GLenum fmt = READ_STACK(2);
    GLenum type = READ_STACK(3);
    const void *pixels = G_PTR(READ_STACK(4));
    if (s_gles_debug_mode) {
        static int s_tex_logged = 0;
        if (s_tex_logged++ < 50) {
            printf("[GL] glTexImage2D target=0x%X level=%d intfmt=0x%X w=%d h=%d fmt=0x%X type=0x%X pix=%p\n",
                   r0, r1, r2, w, h, fmt, type, pixels);
        }
    }
    if (pfn_glTexImage2D) pfn_glTexImage2D(r0, r1, r2, w, h, border, fmt, type, pixels);
    return 0;
}
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
float g_max_anisotropy = 16.0f;

static uint32_t wrap_glTexParameteri(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glTexParameteri) pfn_glTexParameteri(r0, r1, r2);
    if (g_max_anisotropy > 1.0f && r0 == 0x0DE1 /* GL_TEXTURE_2D */ && r1 == 0x2801 /* GL_TEXTURE_MIN_FILTER */) {
        if (pfn_glTexParameterf) {
            pfn_glTexParameterf(r0, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_max_anisotropy);
        }
    }
    return 0;
}

static uint32_t wrap_glTexParameterf(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float param;
    memcpy(&param, &r2, 4);
    if (pfn_glTexParameterf) pfn_glTexParameterf(r0, r1, param);
    return 0;
}
static uint32_t wrap_glTexSubImage2D(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLint xoff = r2, yoff = r3;
    GLsizei w = READ_STACK(0);
    GLsizei h = READ_STACK(1);
    GLenum fmt = READ_STACK(2);
    GLenum type = READ_STACK(3);
    const void *pixels = G_PTR(READ_STACK(4));
    if (pfn_glTexSubImage2D) pfn_glTexSubImage2D(r0, r1, xoff, yoff, w, h, fmt, type, pixels);
    return 0;
}
static uint32_t wrap_glUniform1i(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glUniform1i) pfn_glUniform1i(r0, r1);
    return 0;
}
static uint32_t wrap_glUniform4f(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    float v[4];
    memcpy(&v[0], &r1, 4); memcpy(&v[1], &r2, 4); memcpy(&v[2], &r3, 4);
    uint32_t s0 = READ_STACK(0); memcpy(&v[3], &s0, 4);
    if (pfn_glUniform4f) pfn_glUniform4f(r0, v[0], v[1], v[2], v[3]);
    return 0;
}
static uint32_t wrap_glUniform4fv(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glUniform4fv) pfn_glUniform4fv(r0, r1, (const GLfloat*)G_PTR(r2));
    return 0;
}
static uint32_t wrap_glUseProgram(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glUseProgram) pfn_glUseProgram(r0);
    return 0;
}
static uint32_t wrap_glValidateProgram(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    if (pfn_glValidateProgram) pfn_glValidateProgram(r0);
    return 0;
}
static uint32_t wrap_glVertexAttribPointer(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLuint index = r0;
    GLint size = (GLint)r1;
    GLenum type = (GLenum)r2;
    GLboolean norm = (GLboolean)r3;
    GLsizei stride = (GLsizei)READ_STACK(0);
    const void *ptr = (const void*)(uintptr_t)READ_STACK(1);
    GLint bound_vbo = 0;
    if (pfn_glGetIntegerv) pfn_glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &bound_vbo);
    if (!bound_vbo && ptr != NULL) {
        ptr = G_PTR((uint32_t)(uintptr_t)ptr);
    }
    if (pfn_glVertexAttribPointer) pfn_glVertexAttribPointer(index, size, type, norm, stride, ptr);
    return 0;
}
static uint32_t wrap_glViewport(elf32_image_t *img, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t sp) {
    GLint x = (GLint)r0;
    GLint y = (GLint)r1;
    GLsizei w = (GLsizei)r2;
    GLsizei h = (GLsizei)r3;

    if (g_bound_fbo == 0 && s_scale_enabled && !s_off.active) {
        if (w > 0 && h > 0) {
            x = s_dst_x + (GLint)(x * s_scale_x + 0.5f);
            y = s_dst_y + (GLint)(y * s_scale_y + 0.5f);
            w = (GLsizei)(w * s_scale_x + 0.5f);
            h = (GLsizei)(h * s_scale_y + 0.5f);
        }
    }

    if (s_gles_debug_mode) {
        static int s_vp_logged = 0;
        if (s_vp_logged++ < 10) {
            printf("[GL] glViewport(x=%d, y=%d, w=%d, h=%d)%s\n",
                   x, y, w, h, (g_bound_fbo == 0 && s_scale_enabled && !s_off.active) ? " [SCALED]" : "");
        }
    }
    if (pfn_glViewport) pfn_glViewport(x, y, w, h);
    return 0;
}

struct GlesDispatch {
    const char *name;
    svc_handler_fn fn;
};

static const struct GlesDispatch s_gles_table[] = {
    {"glActiveTexture", wrap_glActiveTexture},
    {"glAttachShader", wrap_glAttachShader},
    {"glBindAttribLocation", wrap_glBindAttribLocation},
    {"glBindBuffer", wrap_glBindBuffer},
    {"glBindFramebuffer", wrap_glBindFramebuffer},
    {"glBindRenderbuffer", wrap_glBindRenderbuffer},
    {"glBindTexture", wrap_glBindTexture},
    {"glBlendFunc", wrap_glBlendFunc},
    {"glBufferData", wrap_glBufferData},
    {"glBufferSubData", wrap_glBufferSubData},
    {"glCheckFramebufferStatus", wrap_glCheckFramebufferStatus},
    {"glClear", wrap_glClear},
    {"glClearColor", wrap_glClearColor},
    {"glClearDepthf", wrap_glClearDepthf},
    {"glClearStencil", wrap_glClearStencil},
    {"glColorMask", wrap_glColorMask},
    {"glCompileShader", wrap_glCompileShader},
    {"glCompressedTexImage2D", wrap_glCompressedTexImage2D},
    {"glCopyTexImage2D", wrap_glCopyTexImage2D},
    {"glCopyTexSubImage2D", wrap_glCopyTexSubImage2D},
    {"glCreateProgram", wrap_glCreateProgram},
    {"glCreateShader", wrap_glCreateShader},
    {"glCullFace", wrap_glCullFace},
    {"glDeleteBuffers", wrap_glDeleteBuffers},
    {"glDeleteFramebuffers", wrap_glDeleteFramebuffers},
    {"glDeleteProgram", wrap_glDeleteProgram},
    {"glDeleteRenderbuffers", wrap_glDeleteRenderbuffers},
    {"glDeleteShader", wrap_glDeleteShader},
    {"glDeleteTextures", wrap_glDeleteTextures},
    {"glDepthFunc", wrap_glDepthFunc},
    {"glDepthMask", wrap_glDepthMask},
    {"glDisable", wrap_glDisable},
    {"glDisableVertexAttribArray", wrap_glDisableVertexAttribArray},
    {"glDrawArrays", wrap_glDrawArrays},
    {"glDrawElements", wrap_glDrawElements},
    {"glEnable", wrap_glEnable},
    {"glEnableVertexAttribArray", wrap_glEnableVertexAttribArray},
    {"glFinish", wrap_glFinish},
    {"glFlush", wrap_glFlush},
    {"glFramebufferRenderbuffer", wrap_glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", wrap_glFramebufferTexture2D},
    {"glGenBuffers", wrap_glGenBuffers},
    {"glGenFramebuffers", wrap_glGenFramebuffers},
    {"glGenRenderbuffers", wrap_glGenRenderbuffers},
    {"glGenTextures", wrap_glGenTextures},
    {"glGetBufferParameteriv", wrap_glGetBufferParameteriv},
    {"glGetFloatv", wrap_glGetFloatv},
    {"glGetIntegerv", wrap_glGetIntegerv},
    {"glGetProgramInfoLog", wrap_glGetProgramInfoLog},
    {"glGetProgramiv", wrap_glGetProgramiv},
    {"glGetShaderInfoLog", wrap_glGetShaderInfoLog},
    {"glGetShaderiv", wrap_glGetShaderiv},
    {"glGetString", wrap_glGetString},
    {"glGetUniformLocation", wrap_glGetUniformLocation},
    {"glLinkProgram", wrap_glLinkProgram},
    {"glPixelStorei", wrap_glPixelStorei},
    {"glPolygonOffset", wrap_glPolygonOffset},
    {"glReadPixels", wrap_glReadPixels},
    {"glRenderbufferStorage", wrap_glRenderbufferStorage},
    {"glScissor", wrap_glScissor},
    {"glShaderSource", wrap_glShaderSource},
    {"glStencilFunc", wrap_glStencilFunc},
    {"glStencilMask", wrap_glStencilMask},
    {"glStencilOp", wrap_glStencilOp},
    {"glTexImage2D", wrap_glTexImage2D},
    {"glTexParameterf", wrap_glTexParameterf},
    {"glTexParameteri", wrap_glTexParameteri},
    {"glTexSubImage2D", wrap_glTexSubImage2D},
    {"glUniform1i", wrap_glUniform1i},
    {"glUniform4f", wrap_glUniform4f},
    {"glUniform4fv", wrap_glUniform4fv},
    {"glUseProgram", wrap_glUseProgram},
    {"glValidateProgram", wrap_glValidateProgram},
    {"glVertexAttribPointer", wrap_glVertexAttribPointer},
    {"glViewport", wrap_glViewport},
    {NULL, NULL}
};

svc_handler_fn bridge_gles_lookup(const char *name) {
    for (int i = 0; s_gles_table[i].name != NULL; ++i) {
        if (strcmp(s_gles_table[i].name, name) == 0) {
            return s_gles_table[i].fn;
        }
    }
    return NULL;
}

void bridge_gles_get_frame_stats(uint32_t *draw_arrays, uint32_t *draw_elements, uint32_t *clears, uint32_t *fbo) {
    if (draw_arrays) *draw_arrays = g_draw_arrays_calls;
    if (draw_elements) *draw_elements = g_draw_elements_calls;
    if (clears) *clears = g_clear_calls;
    if (fbo) *fbo = g_bound_fbo;
}

void bridge_gles_reset_frame_stats(void) {
    g_draw_arrays_calls = 0;
    g_draw_elements_calls = 0;
    g_clear_calls = 0;
}

uint32_t bridge_gles_get_bound_fbo(void) {
    return g_bound_fbo;
}

void bridge_gles_save_screenshot(const char *filename, int w, int h) {
    int off_screenshot = s_off.active && g_bound_fbo == 0;
    if (off_screenshot) {
        /* Capture the offscreen frame at render resolution */
        w = s_off.w;
        h = s_off.h;
        off_resolve();
        pfn_glBindFramebuffer(GL_READ_FRAMEBUFFER_, off_display_fbo());
    }
    if (!pfn_glReadPixels) {
        printf("[-] Screenshot failed: glReadPixels not loaded\n");
        return;
    }
    uint8_t *rgba = (uint8_t*)calloc(w * h, 4);
    if (!rgba) return;

    GLint cur_fbo = 0;
    if (pfn_glGetIntegerv) pfn_glGetIntegerv(0x8CA6 /* GL_FRAMEBUFFER_BINDING */, &cur_fbo);

    if (pfn_glPixelStorei) pfn_glPixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
    pfn_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, rgba);

    if (off_screenshot) pfn_glBindFramebuffer(GL_READ_FRAMEBUFFER_, s_off.main_fbo);

    GLenum err = pfn_glGetError ? pfn_glGetError() : 0;
    if (err != 0) {
        printf("[-] glReadPixels error: 0x%04X (fbo=%d)\n", err, cur_fbo);
    }

    uint64_t sum_rgb = 0;
    uint32_t non_black_px = 0;
    uint8_t min_c = 255, max_c = 0;
    for (int i = 0; i < w * h; ++i) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        if (r > 0 || g > 0 || b > 0) non_black_px++;
        sum_rgb += (uint64_t)r + g + b;
        if (r < min_c) min_c = r;
        if (r > max_c) max_c = r;
        if (g < min_c) min_c = g;
        if (g > max_c) max_c = g;
        if (b < min_c) min_c = b;
        if (b > max_c) max_c = b;
    }
    if (s_gles_debug_mode) {
        printf("[+] Screenshot check '%s': non-black px = %u / %d, sum_rgb = %llu, min=%u max=%u (fbo=%d)\n",
               filename, non_black_px, w * h, (unsigned long long)sum_rgb, min_c, max_c, cur_fbo);
    }

    FILE *f = fopen(filename, "wb");
    if (f) {
        int row_stride = (w * 3 + 3) & ~3;
        int img_size = row_stride * h;
        int file_size = 54 + img_size;
        uint8_t header[54] = {
            'B', 'M',
            (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
            0, 0, 0, 0,
            54, 0, 0, 0,
            40, 0, 0, 0,
            (uint8_t)(w), (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24),
            (uint8_t)(h), (uint8_t)(h >> 8), (uint8_t)(h >> 16), (uint8_t)(h >> 24),
            1, 0,
            24, 0,
            0, 0, 0, 0,
            (uint8_t)(img_size), (uint8_t)(img_size >> 8), (uint8_t)(img_size >> 16), (uint8_t)(img_size >> 24),
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        fwrite(header, 1, 54, f);
        uint8_t *row = (uint8_t*)malloc(row_stride);
        if (row) {
            memset(row, 0, row_stride);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int src_idx = (y * w + x) * 4;
                    row[x * 3 + 0] = rgba[src_idx + 2]; // B
                    row[x * 3 + 1] = rgba[src_idx + 1]; // G
                    row[x * 3 + 2] = rgba[src_idx + 0]; // R
                }
                fwrite(row, 1, row_stride, f);
            }
            free(row);
        }
        fclose(f);
        if (s_gles_debug_mode) {
            printf("[+] Saved screenshot to %s (%dx%d, %d bytes)\n", filename, w, h, file_size);
        }
    }
    free(rgba);
}
