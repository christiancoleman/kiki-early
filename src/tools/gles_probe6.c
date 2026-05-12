/* gles_probe6.c — render-to-FBO then sample. Pass 1: render green quad
 * into an FBO-backed texture. Pass 2: bind that texture, render textured
 * quad to the default pbuffer, read back. Verifies full off-screen path. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int   EGLint;
typedef unsigned EGLBoolean, EGLenum;
typedef unsigned GLbitfield, GLenum, GLuint;
typedef int      GLint, GLsizei;
typedef float    GLfloat;
typedef unsigned char GLboolean;

#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_CONTEXT      ((void*)0)
#define EGL_NONE                       0x3038
#define EGL_SURFACE_TYPE               0x3033
#define EGL_PBUFFER_BIT                0x0001
#define EGL_RED_SIZE                   0x3024
#define EGL_GREEN_SIZE                 0x3023
#define EGL_BLUE_SIZE                  0x3022
#define EGL_ALPHA_SIZE                 0x3021
#define EGL_RENDERABLE_TYPE            0x3040
#define EGL_OPENGL_ES2_BIT             0x0004
#define EGL_OPENGL_ES_API              0x30A0
#define EGL_CONTEXT_CLIENT_VERSION     0x3098
#define EGL_WIDTH                      0x3057
#define EGL_HEIGHT                     0x3056

#define GL_NO_ERROR          0
#define GL_COLOR_BUFFER_BIT  0x4000
#define GL_RGBA              0x1908
#define GL_UNSIGNED_BYTE     0x1401
#define GL_FLOAT             0x1406
#define GL_FALSE             0
#define GL_VERTEX_SHADER     0x8B31
#define GL_FRAGMENT_SHADER   0x8B30
#define GL_COMPILE_STATUS    0x8B81
#define GL_LINK_STATUS       0x8B82
#define GL_ARRAY_BUFFER      0x8892
#define GL_STATIC_DRAW       0x88E4
#define GL_TRIANGLES         0x0004
#define GL_TEXTURE_2D        0x0DE1
#define GL_TEXTURE0          0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S    0x2802
#define GL_TEXTURE_WRAP_T    0x2803
#define GL_NEAREST           0x2600
#define GL_CLAMP_TO_EDGE     0x812F
#define GL_FRAMEBUFFER       0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

extern EGLDisplay eglGetDisplay(void *);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
extern EGLBoolean eglBindAPI(EGLenum);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
extern EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern void   glViewport(GLint, GLint, GLsizei, GLsizei);
extern void   glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
extern void   glClear(GLbitfield);
extern void   glFinish(void);
extern void   glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
extern GLuint glCreateShader(GLenum);
extern void   glShaderSource(GLuint, GLsizei, const char *const*, const GLint*);
extern void   glCompileShader(GLuint);
extern void   glGetShaderiv(GLuint, GLenum, GLint*);
extern GLuint glCreateProgram(void);
extern void   glAttachShader(GLuint, GLuint);
extern void   glLinkProgram(GLuint);
extern void   glGetProgramiv(GLuint, GLenum, GLint*);
extern void   glUseProgram(GLuint);
extern void   glGenBuffers(GLsizei, GLuint*);
extern void   glBindBuffer(GLenum, GLuint);
extern void   glBufferData(GLenum, intptr_t, const void*, GLenum);
extern GLint  glGetAttribLocation(GLuint, const char*);
extern GLint  glGetUniformLocation(GLuint, const char*);
extern void   glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
extern void   glEnableVertexAttribArray(GLuint);
extern void   glDrawArrays(GLenum, GLint, GLsizei);
extern void   glGenTextures(GLsizei, GLuint*);
extern void   glActiveTexture(GLenum);
extern void   glBindTexture(GLenum, GLuint);
extern void   glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
extern void   glTexParameteri(GLenum, GLenum, GLint);
extern void   glUniform1i(GLint, GLint);
extern void   glGenFramebuffers(GLsizei, GLuint*);
extern void   glBindFramebuffer(GLenum, GLuint);
extern void   glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
extern GLenum glCheckFramebufferStatus(GLenum);

#define W 64
#define H 32

static const char *vs_solid_src =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_solid_src =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";  /* green */

static const char *vs_tex_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
static const char *fs_tex_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D s_tex;\n"
    "void main() { gl_FragColor = texture2D(s_tex, v_uv); }\n";

static GLuint make_program(const char *vs_src, const char *fs_src) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL); glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL); glCompileShader(fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    return p;
}

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min; eglInitialize(dpy, &maj, &min); eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig cfg; EGLint n; eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n);
    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    EGLint pb_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
    eglMakeCurrent(dpy, surf, surf, ctx);

    /* Pass 1: render green quad into an FBO-backed texture. */
    GLuint solid = make_program(vs_solid_src, fs_solid_src);
    GLuint tex; glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint fbo; glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    fprintf(stderr, "FBO status=0x%x (COMPLETE=0x%x)\n", fbstatus, GL_FRAMEBUFFER_COMPLETE);

    glUseProgram(solid);
    GLfloat full_pos[12] = {
        -1,-1, 1,-1, -1,1,  1,-1, 1,1, -1,1
    };
    GLuint vbo_pos; glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(full_pos), full_pos, GL_STATIC_DRAW);
    GLint a_pos = glGetAttribLocation(solid, "a_pos");
    glEnableVertexAttribArray((GLuint)a_pos);
    glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    /* Pass 2: bind default fb, sample fbo's tex. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLuint texprog = make_program(vs_tex_src, fs_tex_src);
    glUseProgram(texprog);
    GLfloat full_uv[24] = {
        -1,-1, 0,0,  1,-1, 1,0,  -1,1, 0,1,
         1,-1, 1,0,  1,1, 1,1,  -1,1, 0,1,
    };
    GLuint vbo_uv; glGenBuffers(1, &vbo_uv);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_uv);
    glBufferData(GL_ARRAY_BUFFER, sizeof(full_uv), full_uv, GL_STATIC_DRAW);
    GLint p2_pos = glGetAttribLocation(texprog, "a_pos");
    GLint p2_uv  = glGetAttribLocation(texprog, "a_uv");
    glEnableVertexAttribArray((GLuint)p2_pos);
    glEnableVertexAttribArray((GLuint)p2_uv);
    glVertexAttribPointer((GLuint)p2_pos, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glVertexAttribPointer((GLuint)p2_uv,  2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint sloc = glGetUniformLocation(texprog, "s_tex");
    glUniform1i(sloc, 0);
    glViewport(0, 0, W, H);
    glClearColor(0.5f, 0.0f, 0.5f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    unsigned char pixels[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int green = 0;
    for (int i = 0; i < W * H * 4; i += 4) {
        if (pixels[i] == 0 && pixels[i+1] == 255 && pixels[i+2] == 0 && pixels[i+3] == 255)
            green++;
    }
    fprintf(stderr, "first pixel: (%u,%u,%u,%u)\n", pixels[0], pixels[1], pixels[2], pixels[3]);
    fprintf(stderr, "green pixels: %d/%d\n", green, W * H);
    if (green == W * H) {
        fprintf(stderr, "VERDICT: ALL PASS — FBO render-target + sample through daemon works\n");
        return 0;
    }
    return 1;
}
