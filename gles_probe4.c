/* gles_probe4.c — full triangle pipeline through shim+daemon. */
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
#define EGL_NO_DISPLAY      ((void*)0)
#define EGL_NO_SURFACE      ((void*)0)
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
extern void   glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
extern void   glEnableVertexAttribArray(GLuint);
extern void   glDrawArrays(GLenum, GLint, GLsizei);

#define W 64
#define H 32

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    eglInitialize(dpy, &maj, &min);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig cfg; EGLint n;
    eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n);
    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    EGLint pb_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
    eglMakeCurrent(dpy, surf, surf, ctx);

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLint ok = 0; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    fprintf(stderr, "vs compile: %d\n", ok);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    fprintf(stderr, "fs compile: %d\n", ok);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    fprintf(stderr, "link: %d  prog=%u\n", ok, prog);

    glUseProgram(prog);

    /* Full-quad covering whole pbuffer using 2 triangles */
    GLfloat verts[12] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,
         1.0f, -1.0f,  1.0f,  1.0f,  -1.0f, 1.0f,
    };
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    GLint a_pos = glGetAttribLocation(prog, "a_pos");
    fprintf(stderr, "a_pos = %d\n", a_pos);
    glEnableVertexAttribArray((GLuint)a_pos);
    glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);  /* blue clear */
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    unsigned char pixels[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int green_count = 0;
    for (int i = 0; i < W * H * 4; i += 4) {
        if (pixels[i] == 0 && pixels[i+1] == 255 && pixels[i+2] == 0 && pixels[i+3] == 255)
            green_count++;
    }
    fprintf(stderr, "first pixel: (%u,%u,%u,%u)\n",
            pixels[0], pixels[1], pixels[2], pixels[3]);
    fprintf(stderr, "green pixels: %d/%d\n", green_count, W * H);
    if (green_count == W * H) {
        fprintf(stderr, "VERDICT: ALL PASS — full triangle pipeline through daemon works\n");
        return 0;
    }
    fprintf(stderr, "FAIL: not all pixels are green\n");
    return 1;
}
