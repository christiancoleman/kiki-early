/* Harder aarch64 GLES3 render probe: multiple clears + a real shaded triangle.
 * Each test is independent so we can see partial-credit outcomes if something breaks. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef unsigned char GLubyte;

#define EGL_DEFAULT_DISPLAY          ((void*)0)
#define EGL_NO_CONTEXT               ((EGLContext)0)
#define EGL_NONE                     0x3038
#define EGL_RED_SIZE                 0x3024
#define EGL_GREEN_SIZE               0x3023
#define EGL_BLUE_SIZE                0x3022
#define EGL_ALPHA_SIZE               0x3021
#define EGL_DEPTH_SIZE               0x3025
#define EGL_SURFACE_TYPE             0x3033
#define EGL_RENDERABLE_TYPE          0x3040
#define EGL_PBUFFER_BIT              0x0001
#define EGL_OPENGL_ES3_BIT           0x0040
#define EGL_OPENGL_ES_API            0x30A0
#define EGL_WIDTH                    0x3057
#define EGL_HEIGHT                   0x3056
#define EGL_CONTEXT_CLIENT_VERSION   0x3098
#define EGL_VENDOR                   0x3053

#define GL_COLOR_BUFFER_BIT          0x4000
#define GL_RGBA                      0x1908
#define GL_UNSIGNED_BYTE             0x1401
#define GL_VERSION                   0x1F02
#define GL_RENDERER                  0x1F01
#define GL_VENDOR                    0x1F00
#define GL_SHADING_LANGUAGE_VERSION  0x8B8C
#define GL_VERTEX_SHADER             0x8B31
#define GL_FRAGMENT_SHADER           0x8B30
#define GL_COMPILE_STATUS            0x8B81
#define GL_LINK_STATUS               0x8B82
#define GL_ARRAY_BUFFER              0x8892
#define GL_STATIC_DRAW               0x88E4
#define GL_FLOAT                     0x1406
#define GL_FALSE                     0
#define GL_TRIANGLES                 0x0004

typedef EGLDisplay  (*PFN_eglGetDisplay)(void*);
typedef EGLBoolean  (*PFN_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean  (*PFN_eglBindAPI)(EGLint);
typedef EGLBoolean  (*PFN_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLSurface  (*PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLContext  (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean  (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint      (*PFN_eglGetError)(void);
typedef const char *(*PFN_eglQueryString)(EGLDisplay, EGLint);

typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef void           (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void           (*PFN_glClear)(GLbitfield);
typedef void           (*PFN_glFinish)(void);
typedef void           (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef GLenum         (*PFN_glGetError)(void);
typedef void           (*PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef GLuint         (*PFN_glCreateShader)(GLenum);
typedef void           (*PFN_glShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void           (*PFN_glCompileShader)(GLuint);
typedef void           (*PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void           (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
typedef GLuint         (*PFN_glCreateProgram)(void);
typedef void           (*PFN_glAttachShader)(GLuint, GLuint);
typedef void           (*PFN_glLinkProgram)(GLuint);
typedef void           (*PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void           (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
typedef void           (*PFN_glUseProgram)(GLuint);
typedef GLint          (*PFN_glGetAttribLocation)(GLuint, const char*);
typedef void           (*PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void           (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void           (*PFN_glBufferData)(GLenum, long, const void*, GLenum);
typedef void           (*PFN_glEnableVertexAttribArray)(GLuint);
typedef void           (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLubyte, GLsizei, const void*);
typedef void           (*PFN_glDrawArrays)(GLenum, GLint, GLsizei);

#define LOAD(VAR, H, NAME) do {                                     \
    VAR = (void*)dlsym((H), (NAME));                                \
    if (!VAR) { fprintf(stderr, "dlsym %s fail: %s\n",              \
                        (NAME), dlerror()); exit(9); }              \
} while(0)

static int iabs(int a){ return a<0?-a:a; }

static PFN_glReadPixels pf_glReadPixels;
static PFN_glClearColor pf_glClearColor;
static PFN_glClear pf_glClear;
static PFN_glFinish pf_glFinish;
static PFN_glGetError pf_glGetError;

static int check_pixel(const char *label, GLint x, GLint y,
                       int tr, int tg, int tb, int ta, int tol) {
    unsigned char pix[4] = {0};
    pf_glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    int ok = (iabs(pix[0]-tr)<=tol && iabs(pix[1]-tg)<=tol &&
              iabs(pix[2]-tb)<=tol && iabs(pix[3]-ta)<=tol);
    printf("  %-24s at(%2d,%2d) expect=%02x %02x %02x %02x  got=%02x %02x %02x %02x  %s\n",
           label, x, y, tr, tg, tb, ta, pix[0], pix[1], pix[2], pix[3],
           ok ? "OK" : "FAIL");
    return ok;
}

int main(void) {
    /* --- EGL setup (identical to gles_probe.c) --- */
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!egl) { fprintf(stderr, "dlopen libEGL: %s\n", dlerror()); return 1; }
    PFN_eglGetDisplay pf_eglGetDisplay; LOAD(pf_eglGetDisplay, egl, "eglGetDisplay");
    PFN_eglInitialize pf_eglInitialize; LOAD(pf_eglInitialize, egl, "eglInitialize");
    PFN_eglBindAPI pf_eglBindAPI; LOAD(pf_eglBindAPI, egl, "eglBindAPI");
    PFN_eglChooseConfig pf_eglChooseConfig; LOAD(pf_eglChooseConfig, egl, "eglChooseConfig");
    PFN_eglCreatePbufferSurface pf_eglCreatePbufferSurface; LOAD(pf_eglCreatePbufferSurface, egl, "eglCreatePbufferSurface");
    PFN_eglCreateContext pf_eglCreateContext; LOAD(pf_eglCreateContext, egl, "eglCreateContext");
    PFN_eglMakeCurrent pf_eglMakeCurrent; LOAD(pf_eglMakeCurrent, egl, "eglMakeCurrent");
    PFN_eglGetError pf_eglGetError; LOAD(pf_eglGetError, egl, "eglGetError");
    PFN_eglQueryString pf_eglQueryString; LOAD(pf_eglQueryString, egl, "eglQueryString");

    EGLDisplay d = pf_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj=0, min=0;
    if (!pf_eglInitialize(d, &maj, &min)) { fprintf(stderr, "eglInit err=0x%x\n", pf_eglGetError()); return 2; }
    pf_eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n_cfg = 0;
    pf_eglChooseConfig(d, cfg_attrs, &cfg, 1, &n_cfg);
    EGLint pb_attrs[] = { EGL_WIDTH, 128, EGL_HEIGHT, 128, EGL_NONE };
    EGLSurface surf = pf_eglCreatePbufferSurface(d, cfg, pb_attrs);
    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = pf_eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attrs);
    pf_eglMakeCurrent(d, surf, surf, ctx);

    /* --- GLES --- */
    void *gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!gl) { fprintf(stderr, "dlopen libGLESv2: %s\n", dlerror()); return 3; }
    PFN_glGetString pf_glGetString; LOAD(pf_glGetString, gl, "glGetString");
    LOAD(pf_glClearColor, gl, "glClearColor");
    LOAD(pf_glClear, gl, "glClear");
    LOAD(pf_glFinish, gl, "glFinish");
    LOAD(pf_glReadPixels, gl, "glReadPixels");
    LOAD(pf_glGetError, gl, "glGetError");
    PFN_glViewport pf_glViewport; LOAD(pf_glViewport, gl, "glViewport");
    PFN_glCreateShader pf_glCreateShader; LOAD(pf_glCreateShader, gl, "glCreateShader");
    PFN_glShaderSource pf_glShaderSource; LOAD(pf_glShaderSource, gl, "glShaderSource");
    PFN_glCompileShader pf_glCompileShader; LOAD(pf_glCompileShader, gl, "glCompileShader");
    PFN_glGetShaderiv pf_glGetShaderiv; LOAD(pf_glGetShaderiv, gl, "glGetShaderiv");
    PFN_glGetShaderInfoLog pf_glGetShaderInfoLog; LOAD(pf_glGetShaderInfoLog, gl, "glGetShaderInfoLog");
    PFN_glCreateProgram pf_glCreateProgram; LOAD(pf_glCreateProgram, gl, "glCreateProgram");
    PFN_glAttachShader pf_glAttachShader; LOAD(pf_glAttachShader, gl, "glAttachShader");
    PFN_glLinkProgram pf_glLinkProgram; LOAD(pf_glLinkProgram, gl, "glLinkProgram");
    PFN_glGetProgramiv pf_glGetProgramiv; LOAD(pf_glGetProgramiv, gl, "glGetProgramiv");
    PFN_glGetProgramInfoLog pf_glGetProgramInfoLog; LOAD(pf_glGetProgramInfoLog, gl, "glGetProgramInfoLog");
    PFN_glUseProgram pf_glUseProgram; LOAD(pf_glUseProgram, gl, "glUseProgram");
    PFN_glGetAttribLocation pf_glGetAttribLocation; LOAD(pf_glGetAttribLocation, gl, "glGetAttribLocation");
    PFN_glGenBuffers pf_glGenBuffers; LOAD(pf_glGenBuffers, gl, "glGenBuffers");
    PFN_glBindBuffer pf_glBindBuffer; LOAD(pf_glBindBuffer, gl, "glBindBuffer");
    PFN_glBufferData pf_glBufferData; LOAD(pf_glBufferData, gl, "glBufferData");
    PFN_glEnableVertexAttribArray pf_glEnableVertexAttribArray; LOAD(pf_glEnableVertexAttribArray, gl, "glEnableVertexAttribArray");
    PFN_glVertexAttribPointer pf_glVertexAttribPointer; LOAD(pf_glVertexAttribPointer, gl, "glVertexAttribPointer");
    PFN_glDrawArrays pf_glDrawArrays; LOAD(pf_glDrawArrays, gl, "glDrawArrays");

    printf("EGL: %d.%d  %s\n", maj, min, pf_eglQueryString(d, EGL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char*)pf_glGetString(GL_RENDERER));
    printf("GL_VERSION:  %s\n", (const char*)pf_glGetString(GL_VERSION));
    printf("GL_GLSL_VER: %s\n\n", (const char*)pf_glGetString(GL_SHADING_LANGUAGE_VERSION));

    pf_glViewport(0, 0, 128, 128);

    /* ========= Test 1: glClear with three unrelated colors ========= */
    printf("Test 1: three distinct glClear colors\n");
    int pass = 0, total = 0;
    struct { const char *name; GLfloat r, g, b, a; int tr, tg, tb, ta; } clears[] = {
        { "clear #1 purple", 0.75f, 0.1f,  0.9f,  1.0f, 0xbf, 0x19, 0xe6, 0xff },
        { "clear #2 teal",   0.2f,  0.8f,  0.3f,  1.0f, 0x33, 0xcc, 0x4c, 0xff },
        { "clear #3 black",  0.0f,  0.0f,  0.0f,  1.0f, 0x00, 0x00, 0x00, 0xff },
        { "clear #4 white",  1.0f,  1.0f,  1.0f,  0.25f,0xff, 0xff, 0xff, 0x40 },
    };
    for (unsigned i = 0; i < sizeof(clears)/sizeof(clears[0]); i++) {
        pf_glClearColor(clears[i].r, clears[i].g, clears[i].b, clears[i].a);
        pf_glClear(GL_COLOR_BUFFER_BIT);
        pf_glFinish();
        total++;
        pass += check_pixel(clears[i].name, 64, 64,
                            clears[i].tr, clears[i].tg, clears[i].tb, clears[i].ta, 2);
    }
    printf("  subtotal: %d/%d\n\n", pass, total);

    /* ========= Test 2: vertex/fragment shader + triangle draw ========= */
    printf("Test 2: shaded triangle (shader compile + link + draw)\n");
    const char *vs_src =
        "#version 300 es\n"
        "in vec2 pos;\n"
        "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    const char *fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "out vec4 fc;\n"
        "void main() { fc = vec4(0.9, 0.2, 0.6, 1.0); }\n";  /* ≈ e6,33,99,ff */

    GLuint vs = pf_glCreateShader(GL_VERTEX_SHADER);
    pf_glShaderSource(vs, 1, &vs_src, NULL);
    pf_glCompileShader(vs);
    GLint s = 0; pf_glGetShaderiv(vs, GL_COMPILE_STATUS, &s);
    if (!s) { char log[1024]={0}; pf_glGetShaderInfoLog(vs, 1024, NULL, log); fprintf(stderr, "VS compile FAIL: %s\n", log); return 20; }
    printf("  vertex shader compile: OK\n");

    GLuint fs = pf_glCreateShader(GL_FRAGMENT_SHADER);
    pf_glShaderSource(fs, 1, &fs_src, NULL);
    pf_glCompileShader(fs);
    pf_glGetShaderiv(fs, GL_COMPILE_STATUS, &s);
    if (!s) { char log[1024]={0}; pf_glGetShaderInfoLog(fs, 1024, NULL, log); fprintf(stderr, "FS compile FAIL: %s\n", log); return 21; }
    printf("  fragment shader compile: OK\n");

    GLuint prog = pf_glCreateProgram();
    pf_glAttachShader(prog, vs);
    pf_glAttachShader(prog, fs);
    pf_glLinkProgram(prog);
    pf_glGetProgramiv(prog, GL_LINK_STATUS, &s);
    if (!s) { char log[1024]={0}; pf_glGetProgramInfoLog(prog, 1024, NULL, log); fprintf(stderr, "Link FAIL: %s\n", log); return 22; }
    printf("  program link: OK\n");

    pf_glUseProgram(prog);

    /* Triangle covering the center of the pbuffer (from NDC -0.8..0.8 in X, -0.8..0.8 in Y) */
    GLfloat verts[] = {
        -0.8f, -0.8f,
         0.8f, -0.8f,
         0.0f,  0.8f,
    };
    GLuint vbo = 0;
    pf_glGenBuffers(1, &vbo);
    pf_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    pf_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    GLint loc = pf_glGetAttribLocation(prog, "pos");
    if (loc < 0) { fprintf(stderr, "attribloc pos FAIL\n"); return 23; }
    pf_glEnableVertexAttribArray((GLuint)loc);
    pf_glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, GL_FALSE, 2*sizeof(GLfloat), (void*)0);

    pf_glClearColor(0.05f, 0.05f, 0.05f, 1.0f);  /* dark so we can tell inside vs outside */
    pf_glClear(GL_COLOR_BUFFER_BIT);
    pf_glDrawArrays(GL_TRIANGLES, 0, 3);
    pf_glFinish();

    /* Expected center-of-buffer ≈ inside triangle at FS output color */
    total++; pass += check_pixel("inside-tri center", 64, 64, 0xe6, 0x33, 0x99, 0xff, 3);
    /* Top-left corner is outside the triangle (y far from center) */
    total++; pass += check_pixel("outside-tri corner", 4, 4, 0x0d, 0x0d, 0x0d, 0xff, 3);

    GLenum e = pf_glGetError();
    if (e) fprintf(stderr, "trailing glGetError=0x%x (nonfatal)\n", e);

    printf("\nTOTAL: %d/%d passes\n", pass, total);
    printf("VERDICT: %s\n", pass == total ? "ALL PASS — llvmpipe is really rasterizing" : "PARTIAL / FAIL");
    return pass == total ? 0 : 33;
}
