/* T035: PC-host surfaceless EGL + OpenGL ES 2.0 offscreen triangle over Mesa Zink -> libvulkan_lodos.so -> vkb_server.
 * Renders a red triangle on a cleared black 256x256 RGBA8 FBO, reads back pixels via glReadPixels,
 * verifies center red + corners black, optionally writes out.ppm, and prints:
 *   VKB zink triangle ok renderer=<str> center=0x.. corner=0x.. */
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

#define DIE(...) do { fprintf(stderr, "HOST_ZINK FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    if (!sh) DIE("glCreateShader(0x%x) failed", (unsigned)type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { 0 };
        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        DIE("shader compile failed: %s", log);
    }
    return sh;
}

static void write_ppm(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) DIE("fopen(%s) failed", path);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t row[W * 3];
    /* Flip Y so PPM top-down matches GL bottom-up coords */
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src = rgba + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        if (fwrite(row, 1, (size_t)w * 3u, f) != (size_t)w * 3u) DIE("fwrite(%s) failed", path);
    }
    if (fclose(f) != 0) DIE("fclose(%s) failed", path);
}

int main(int argc, char **argv) {
    const char *ppm_path = argc > 1 ? argv[1] : NULL;

    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_plat_dpy)
        dpy = get_plat_dpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
        DIE("eglGetDisplay(surfaceless) failed (0x%x)", (unsigned)eglGetError());

    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min))
        DIE("eglInitialize failed (0x%x)", (unsigned)eglGetError());

    if (!eglBindAPI(EGL_OPENGL_ES_API))
        DIE("eglBindAPI(EGL_OPENGL_ES_API) failed (0x%x)", (unsigned)eglGetError());

    const EGLint pb_cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    EGLSurface surf = EGL_NO_SURFACE;
    if (eglChooseConfig(dpy, pb_cfg_attribs, &cfg, 1, &ncfg) && ncfg > 0) {
        const EGLint pb_attribs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
        surf = eglCreatePbufferSurface(dpy, cfg, pb_attribs);
    } else {
        const EGLint sl_cfg_attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        if (!eglChooseConfig(dpy, sl_cfg_attribs, &cfg, 1, &ncfg) || ncfg < 1)
            DIE("eglChooseConfig failed (0x%x)", (unsigned)eglGetError());
    }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT)
        DIE("eglCreateContext failed (0x%x)", (unsigned)eglGetError());

    if (!eglMakeCurrent(dpy, surf, surf, ctx))
        DIE("eglMakeCurrent failed (0x%x)", (unsigned)eglGetError());

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    if (!renderer) DIE("glGetString(GL_RENDERER) returned NULL");
    if (!strstr(renderer, "zink"))
        DIE("renderer is not zink: %s (vendor=%s version=%s)", renderer, vendor ? vendor : "?", version ? version : "?");
    if (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe"))
        DIE("unexpected software rasterizer: %s", renderer);

    /* Explicit 256x256 RGBA8 texture-backed FBO */
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fb_st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fb_st != GL_FRAMEBUFFER_COMPLETE)
        DIE("FBO incomplete: 0x%x", (unsigned)fb_st);

    static const char *const k_vs =
        "attribute vec2 aPos;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    static const char *const k_fs =
        "precision mediump float;\n"
        "void main() {\n"
        "    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "}\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, k_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = { 0 };
        glGetProgramInfoLog(prog, sizeof(log) - 1, NULL, log);
        DIE("program link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    static const GLfloat verts[6] = {
         0.0f,  0.6f,
        -0.6f, -0.6f,
         0.6f, -0.6f,
    };
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glViewport(0, 0, W, H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * (GLsizei)sizeof(GLfloat), (const void *)0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        DIE("GL error before readback: 0x%x", (unsigned)err);

    uint8_t *px = malloc((size_t)W * H * 4u);
    if (!px) DIE("malloc failed");
    memset(px, 0x5a, (size_t)W * H * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    err = glGetError();
    if (err != GL_NO_ERROR)
        DIE("glReadPixels error: 0x%x", (unsigned)err);

    const uint8_t *center = px + ((size_t)(H / 2) * W + (W / 2)) * 4u;
    const uint8_t *c00 = px;
    const uint8_t *c10 = px + (size_t)(W - 1) * 4u;
    const uint8_t *c01 = px + (size_t)(H - 1) * W * 4u;
    const uint8_t *c11 = px + ((size_t)(H - 1) * W + (W - 1)) * 4u;

    if (center[0] < 250 || center[1] > 5 || center[2] > 5 || center[3] < 250)
        DIE("center pixel not red: (%u,%u,%u,%u)", center[0], center[1], center[2], center[3]);

    const uint8_t *corners[4] = { c00, c10, c01, c11 };
    for (int i = 0; i < 4; i++) {
        const uint8_t *cp = corners[i];
        if (cp[0] > 5 || cp[1] > 5 || cp[2] > 5 || cp[3] < 250)
            DIE("corner[%d] pixel not black: (%u,%u,%u,%u)", i, cp[0], cp[1], cp[2], cp[3]);
    }

    if (ppm_path)
        write_ppm(ppm_path, px, W, H);

    uint8_t cr[4] = { center[0], center[1], center[2], center[3] };
    uint8_t kr[4] = { c00[0], c00[1], c00[2], c00[3] };
    char rend_copy[256];
    snprintf(rend_copy, sizeof(rend_copy), "%s", renderer);

    free(px);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);

    printf("VKB zink triangle ok renderer=%s center=0x%02x%02x%02x%02x corner=0x%02x%02x%02x%02x\n",
           rend_copy, cr[0], cr[1], cr[2], cr[3], kr[0], kr[1], kr[2], kr[3]);
    return 0;
}
