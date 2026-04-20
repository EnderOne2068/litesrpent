/* lsgl.c -- OpenGL bindings for Litesrpent.
 *
 * Dynamically loads opengl32.dll and uses wglGetProcAddress for modern
 * GL functions.  Exposes a comprehensive set of GL functions to Lisp.
 */
#include "lscore.h"
#include "lseval.h"
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* GL type aliases */
typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void GLvoid;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef char GLchar;

/* GL constants */
#define GL_FALSE                0
#define GL_TRUE                 1
#define GL_COLOR_BUFFER_BIT     0x00004000
#define GL_DEPTH_BUFFER_BIT     0x00000100
#define GL_STENCIL_BUFFER_BIT   0x00000400
#define GL_POINTS               0x0000
#define GL_LINES                0x0001
#define GL_LINE_STRIP           0x0003
#define GL_TRIANGLES            0x0004
#define GL_TRIANGLE_STRIP       0x0005
#define GL_TRIANGLE_FAN         0x0006
#define GL_QUADS                0x0007
#define GL_POLYGON              0x0009
#define GL_DEPTH_TEST           0x0B71
#define GL_BLEND                0x0BE2
#define GL_TEXTURE_2D           0x0DE1
#define GL_CULL_FACE            0x0B44
#define GL_MODELVIEW            0x1700
#define GL_PROJECTION           0x1701
#define GL_TEXTURE              0x1702
#define GL_FLOAT                0x1406
#define GL_UNSIGNED_BYTE        0x1401
#define GL_UNSIGNED_INT         0x1405
#define GL_RGB                  0x1907
#define GL_RGBA                 0x1908
#define GL_NEAREST              0x2600
#define GL_LINEAR               0x2601
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_REPEAT               0x2901
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_NO_ERROR             0
#define GL_SRC_ALPHA            0x0302
#define GL_ONE_MINUS_SRC_ALPHA  0x0303
#define GL_VERTEX_SHADER        0x8B31
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_ARRAY_BUFFER         0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW          0x88E4
#define GL_DYNAMIC_DRAW         0x88E8
#define GL_FRAMEBUFFER          0x8D40

/* Function pointer types for GL 1.x */
typedef void   (APIENTRY *PFNGLCLEARCOLOR)(GLclampf,GLclampf,GLclampf,GLclampf);
typedef void   (APIENTRY *PFNGLCLEAR)(GLbitfield);
typedef void   (APIENTRY *PFNGLBEGIN)(GLenum);
typedef void   (APIENTRY *PFNGLEND)(void);
typedef void   (APIENTRY *PFNGLVERTEX2F)(GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLVERTEX3F)(GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLCOLOR3F)(GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLCOLOR4F)(GLfloat,GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLNORMAL3F)(GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLTEXCOORD2F)(GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLVIEWPORT)(GLint,GLint,GLsizei,GLsizei);
typedef void   (APIENTRY *PFNGLMATRIXMODE)(GLenum);
typedef void   (APIENTRY *PFNGLLOADIDENTITY)(void);
typedef void   (APIENTRY *PFNGLORTHO)(GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble);
typedef void   (APIENTRY *PFNGLTRANSLATEF)(GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLROTATEF)(GLfloat,GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLSCALEF)(GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLPUSHMATRIX)(void);
typedef void   (APIENTRY *PFNGLPOPMATRIX)(void);
typedef void   (APIENTRY *PFNGLENABLE)(GLenum);
typedef void   (APIENTRY *PFNGLDISABLE)(GLenum);
typedef void   (APIENTRY *PFNGLBLENDFUNC)(GLenum,GLenum);
typedef void   (APIENTRY *PFNGLGENTEXTURES)(GLsizei,GLuint*);
typedef void   (APIENTRY *PFNGLDELETETEXTURES)(GLsizei,const GLuint*);
typedef void   (APIENTRY *PFNGLBINDTEXTURE)(GLenum,GLuint);
typedef void   (APIENTRY *PFNGLTEXIMAGE2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
typedef void   (APIENTRY *PFNGLTEXPARAMETERI)(GLenum,GLenum,GLint);
typedef void   (APIENTRY *PFNGLFLUSH)(void);
typedef void   (APIENTRY *PFNGLFINISH)(void);
typedef GLenum (APIENTRY *PFNGLGETERROR)(void);
typedef void   (APIENTRY *PFNGLLINEWIDTH)(GLfloat);
typedef void   (APIENTRY *PFNGLPOINTSIZE)(GLfloat);
typedef void   (APIENTRY *PFNGLPOLYGONMODE)(GLenum,GLenum);
typedef void   (APIENTRY *PFNGLSCISSOR)(GLint,GLint,GLsizei,GLsizei);
typedef void   (APIENTRY *PFNGLDEPTHFUNC)(GLenum);
typedef void   (APIENTRY *PFNGLCLEARDEPTH)(GLdouble);
typedef void   (APIENTRY *PFNGLMULTMATRIXF)(const GLfloat*);
typedef void   (APIENTRY *PFNGLFRUSTUM)(GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble);
typedef void   (APIENTRY *PFNGLREADPIXELS)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);

/* Modern GL function types (loaded via wglGetProcAddress) */
typedef GLuint (APIENTRY *PFNGLCREATESHADER)(GLenum);
typedef void   (APIENTRY *PFNGLSHADERSOURCE)(GLuint,GLsizei,const GLchar**,const GLint*);
typedef void   (APIENTRY *PFNGLCOMPILESHADER)(GLuint);
typedef void   (APIENTRY *PFNGLGETSHADERIV)(GLuint,GLenum,GLint*);
typedef void   (APIENTRY *PFNGLGETSHADERINFOLOG)(GLuint,GLsizei,GLsizei*,GLchar*);
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAM)(void);
typedef void   (APIENTRY *PFNGLATTACHSHADER)(GLuint,GLuint);
typedef void   (APIENTRY *PFNGLLINKPROGRAM)(GLuint);
typedef void   (APIENTRY *PFNGLUSEPROGRAM)(GLuint);
typedef void   (APIENTRY *PFNGLGETPROGRAMIV)(GLuint,GLenum,GLint*);
typedef void   (APIENTRY *PFNGLGETPROGRAMINFOLOG)(GLuint,GLsizei,GLsizei*,GLchar*);
typedef GLint  (APIENTRY *PFNGLGETUNIFORMLOCATION)(GLuint,const GLchar*);
typedef void   (APIENTRY *PFNGLUNIFORM1F)(GLint,GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM3F)(GLint,GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM4F)(GLint,GLfloat,GLfloat,GLfloat,GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM1I)(GLint,GLint);
typedef void   (APIENTRY *PFNGLUNIFORMMATRIX4FV)(GLint,GLsizei,GLboolean,const GLfloat*);
typedef void   (APIENTRY *PFNGLGENBUFFERS)(GLsizei,GLuint*);
typedef void   (APIENTRY *PFNGLDELETEBUFFERS)(GLsizei,const GLuint*);
typedef void   (APIENTRY *PFNGLBINDBUFFER)(GLenum,GLuint);
typedef void   (APIENTRY *PFNGLBUFFERDATA)(GLenum,GLsizeiptr,const void*,GLenum);
typedef void   (APIENTRY *PFNGLBUFFERSUBDATA)(GLenum,GLintptr,GLsizeiptr,const void*);
typedef void   (APIENTRY *PFNGLVERTEXATTRIBPOINTER)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);
typedef void   (APIENTRY *PFNGLENABLEVERTEXATTRIBARRAY)(GLuint);
typedef void   (APIENTRY *PFNGLDISABLEVERTEXATTRIBARRAY)(GLuint);
typedef void   (APIENTRY *PFNGLDRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void   (APIENTRY *PFNGLDRAWELEMENTS)(GLenum,GLsizei,GLenum,const void*);
typedef void   (APIENTRY *PFNGLGENVERTEXARRAYS)(GLsizei,GLuint*);
typedef void   (APIENTRY *PFNGLBINDVERTEXARRAY)(GLuint);
typedef void   (APIENTRY *PFNGLDELETEVERTEXARRAYS)(GLsizei,const GLuint*);
typedef void   (APIENTRY *PFNGLDELETESHADER)(GLuint);
typedef void   (APIENTRY *PFNGLDELETEPROGRAM)(GLuint);
typedef void   (APIENTRY *PFNGLGENFRAMEBUFFERS)(GLsizei,GLuint*);
typedef void   (APIENTRY *PFNGLBINDFRAMEBUFFER)(GLenum,GLuint);
typedef void   (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2D)(GLenum,GLenum,GLenum,GLuint,GLint);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void   (APIENTRY *PFNGLDELETEFRAMEBUFFERS)(GLsizei,const GLuint*);

/* WGL */
typedef HGLRC  (WINAPI *PFNWGLCREATECONTEXT)(HDC);
typedef BOOL   (WINAPI *PFNWGLMAKECURRENT)(HDC,HGLRC);
typedef BOOL   (WINAPI *PFNWGLDELETECONTEXT)(HGLRC);
typedef PROC   (WINAPI *PFNWGLGETPROCADDRESS)(LPCSTR);

static HMODULE gl_lib = NULL;

/* GL 1.x functions */
static PFNGLCLEARCOLOR      fn_glClearColor;
static PFNGLCLEAR           fn_glClear;
static PFNGLBEGIN           fn_glBegin;
static PFNGLEND             fn_glEnd;
static PFNGLVERTEX2F        fn_glVertex2f;
static PFNGLVERTEX3F        fn_glVertex3f;
static PFNGLCOLOR3F         fn_glColor3f;
static PFNGLCOLOR4F         fn_glColor4f;
static PFNGLNORMAL3F        fn_glNormal3f;
static PFNGLTEXCOORD2F      fn_glTexCoord2f;
static PFNGLVIEWPORT        fn_glViewport;
static PFNGLMATRIXMODE      fn_glMatrixMode;
static PFNGLLOADIDENTITY    fn_glLoadIdentity;
static PFNGLORTHO           fn_glOrtho;
static PFNGLTRANSLATEF      fn_glTranslatef;
static PFNGLROTATEF         fn_glRotatef;
static PFNGLSCALEF          fn_glScalef;
static PFNGLPUSHMATRIX      fn_glPushMatrix;
static PFNGLPOPMATRIX       fn_glPopMatrix;
static PFNGLENABLE          fn_glEnable;
static PFNGLDISABLE         fn_glDisable;
static PFNGLBLENDFUNC       fn_glBlendFunc;
static PFNGLGENTEXTURES     fn_glGenTextures;
static PFNGLDELETETEXTURES  fn_glDeleteTextures;
static PFNGLBINDTEXTURE     fn_glBindTexture;
static PFNGLTEXIMAGE2D      fn_glTexImage2D;
static PFNGLTEXPARAMETERI   fn_glTexParameteri;
static PFNGLFLUSH           fn_glFlush;
static PFNGLFINISH          fn_glFinish;
static PFNGLGETERROR        fn_glGetError;
static PFNGLLINEWIDTH       fn_glLineWidth;
static PFNGLPOINTSIZE       fn_glPointSize;
static PFNGLDEPTHFUNC       fn_glDepthFunc;
static PFNGLCLEARDEPTH      fn_glClearDepth;
static PFNGLFRUSTUM         fn_glFrustum;
static PFNGLMULTMATRIXF     fn_glMultMatrixf;
static PFNGLREADPIXELS      fn_glReadPixels;

/* Modern GL functions */
static PFNGLCREATESHADER    fn_glCreateShader;
static PFNGLSHADERSOURCE    fn_glShaderSource;
static PFNGLCOMPILESHADER   fn_glCompileShader;
static PFNGLGETSHADERIV     fn_glGetShaderiv;
static PFNGLGETSHADERINFOLOG fn_glGetShaderInfoLog;
static PFNGLCREATEPROGRAM   fn_glCreateProgram;
static PFNGLATTACHSHADER    fn_glAttachShader;
static PFNGLLINKPROGRAM     fn_glLinkProgram;
static PFNGLUSEPROGRAM      fn_glUseProgram;
static PFNGLGETPROGRAMIV    fn_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOG fn_glGetProgramInfoLog;
static PFNGLGETUNIFORMLOCATION fn_glGetUniformLocation;
static PFNGLUNIFORM1F       fn_glUniform1f;
static PFNGLUNIFORM2F       fn_glUniform2f;
static PFNGLUNIFORM3F       fn_glUniform3f;
static PFNGLUNIFORM4F       fn_glUniform4f;
static PFNGLUNIFORM1I       fn_glUniform1i;
static PFNGLUNIFORMMATRIX4FV fn_glUniformMatrix4fv;
static PFNGLGENBUFFERS      fn_glGenBuffers;
static PFNGLDELETEBUFFERS   fn_glDeleteBuffers;
static PFNGLBINDBUFFER      fn_glBindBuffer;
static PFNGLBUFFERDATA      fn_glBufferData;
static PFNGLBUFFERSUBDATA   fn_glBufferSubData;
static PFNGLVERTEXATTRIBPOINTER fn_glVertexAttribPointer;
static PFNGLENABLEVERTEXATTRIBARRAY fn_glEnableVertexAttribArray;
static PFNGLDISABLEVERTEXATTRIBARRAY fn_glDisableVertexAttribArray;
static PFNGLDRAWARRAYS      fn_glDrawArrays;
static PFNGLDRAWELEMENTS    fn_glDrawElements;
static PFNGLGENVERTEXARRAYS fn_glGenVertexArrays;
static PFNGLBINDVERTEXARRAY fn_glBindVertexArray;
static PFNGLDELETEVERTEXARRAYS fn_glDeleteVertexArrays;
static PFNGLDELETESHADER    fn_glDeleteShader;
static PFNGLDELETEPROGRAM   fn_glDeleteProgram;
static PFNGLGENFRAMEBUFFERS fn_glGenFramebuffers;
static PFNGLBINDFRAMEBUFFER fn_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2D fn_glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUS fn_glCheckFramebufferStatus;
static PFNGLDELETEFRAMEBUFFERS fn_glDeleteFramebuffers;

/* WGL */
static PFNWGLCREATECONTEXT  fn_wglCreateContext;
static PFNWGLMAKECURRENT    fn_wglMakeCurrent;
static PFNWGLDELETECONTEXT  fn_wglDeleteContext;
static PFNWGLGETPROCADDRESS fn_wglGetProcAddress;

static int gl_loaded = 0;

static int load_gl(void) {
    if (gl_loaded) return 1;
    gl_lib = LoadLibraryA("opengl32.dll");
    if (!gl_lib) return 0;

#define GL1(name, type) fn_##name = (type)GetProcAddress(gl_lib, #name)
    GL1(glClearColor, PFNGLCLEARCOLOR);
    GL1(glClear, PFNGLCLEAR);
    GL1(glBegin, PFNGLBEGIN);
    GL1(glEnd, PFNGLEND);
    GL1(glVertex2f, PFNGLVERTEX2F);
    GL1(glVertex3f, PFNGLVERTEX3F);
    GL1(glColor3f, PFNGLCOLOR3F);
    GL1(glColor4f, PFNGLCOLOR4F);
    GL1(glNormal3f, PFNGLNORMAL3F);
    GL1(glTexCoord2f, PFNGLTEXCOORD2F);
    GL1(glViewport, PFNGLVIEWPORT);
    GL1(glMatrixMode, PFNGLMATRIXMODE);
    GL1(glLoadIdentity, PFNGLLOADIDENTITY);
    GL1(glOrtho, PFNGLORTHO);
    GL1(glTranslatef, PFNGLTRANSLATEF);
    GL1(glRotatef, PFNGLROTATEF);
    GL1(glScalef, PFNGLSCALEF);
    GL1(glPushMatrix, PFNGLPUSHMATRIX);
    GL1(glPopMatrix, PFNGLPOPMATRIX);
    GL1(glEnable, PFNGLENABLE);
    GL1(glDisable, PFNGLDISABLE);
    GL1(glBlendFunc, PFNGLBLENDFUNC);
    GL1(glGenTextures, PFNGLGENTEXTURES);
    GL1(glDeleteTextures, PFNGLDELETETEXTURES);
    GL1(glBindTexture, PFNGLBINDTEXTURE);
    GL1(glTexImage2D, PFNGLTEXIMAGE2D);
    GL1(glTexParameteri, PFNGLTEXPARAMETERI);
    GL1(glFlush, PFNGLFLUSH);
    GL1(glFinish, PFNGLFINISH);
    GL1(glGetError, PFNGLGETERROR);
    GL1(glLineWidth, PFNGLLINEWIDTH);
    GL1(glPointSize, PFNGLPOINTSIZE);
    GL1(glDepthFunc, PFNGLDEPTHFUNC);
    GL1(glClearDepth, PFNGLCLEARDEPTH);
    GL1(glFrustum, PFNGLFRUSTUM);
    GL1(glMultMatrixf, PFNGLMULTMATRIXF);
    GL1(glReadPixels, PFNGLREADPIXELS);
    GL1(wglCreateContext, PFNWGLCREATECONTEXT);
    GL1(wglMakeCurrent, PFNWGLMAKECURRENT);
    GL1(wglDeleteContext, PFNWGLDELETECONTEXT);
    GL1(wglGetProcAddress, PFNWGLGETPROCADDRESS);
#undef GL1

    gl_loaded = 1;
    return 1;
}

static void load_modern_gl(void) {
    if (!fn_wglGetProcAddress) return;
#define GLE(name, type) fn_##name = (type)fn_wglGetProcAddress(#name)
    GLE(glCreateShader, PFNGLCREATESHADER);
    GLE(glShaderSource, PFNGLSHADERSOURCE);
    GLE(glCompileShader, PFNGLCOMPILESHADER);
    GLE(glGetShaderiv, PFNGLGETSHADERIV);
    GLE(glGetShaderInfoLog, PFNGLGETSHADERINFOLOG);
    GLE(glCreateProgram, PFNGLCREATEPROGRAM);
    GLE(glAttachShader, PFNGLATTACHSHADER);
    GLE(glLinkProgram, PFNGLLINKPROGRAM);
    GLE(glUseProgram, PFNGLUSEPROGRAM);
    GLE(glGetProgramiv, PFNGLGETPROGRAMIV);
    GLE(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOG);
    GLE(glGetUniformLocation, PFNGLGETUNIFORMLOCATION);
    GLE(glUniform1f, PFNGLUNIFORM1F);
    GLE(glUniform2f, PFNGLUNIFORM2F);
    GLE(glUniform3f, PFNGLUNIFORM3F);
    GLE(glUniform4f, PFNGLUNIFORM4F);
    GLE(glUniform1i, PFNGLUNIFORM1I);
    GLE(glUniformMatrix4fv, PFNGLUNIFORMMATRIX4FV);
    GLE(glGenBuffers, PFNGLGENBUFFERS);
    GLE(glDeleteBuffers, PFNGLDELETEBUFFERS);
    GLE(glBindBuffer, PFNGLBINDBUFFER);
    GLE(glBufferData, PFNGLBUFFERDATA);
    GLE(glBufferSubData, PFNGLBUFFERSUBDATA);
    GLE(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTER);
    GLE(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAY);
    GLE(glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAY);
    GLE(glDrawArrays, PFNGLDRAWARRAYS);
    GLE(glDrawElements, PFNGLDRAWELEMENTS);
    GLE(glGenVertexArrays, PFNGLGENVERTEXARRAYS);
    GLE(glBindVertexArray, PFNGLBINDVERTEXARRAY);
    GLE(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYS);
    GLE(glDeleteShader, PFNGLDELETESHADER);
    GLE(glDeleteProgram, PFNGLDELETEPROGRAM);
    GLE(glGenFramebuffers, PFNGLGENFRAMEBUFFERS);
    GLE(glBindFramebuffer, PFNGLBINDFRAMEBUFFER);
    GLE(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2D);
    GLE(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUS);
    GLE(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERS);
#undef GLE
}

/* ---- GL context creation ---- */
static ls_value_t bi_gl_create_context(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || a[0].tag != LS_T_FOREIGN) { ls_error(L, "gl-create-context: need hwnd"); return ls_nil_v(); }
    if (!load_gl()) { ls_error(L, "cannot load opengl32.dll"); return ls_nil_v(); }
    HWND hwnd = (HWND)((ls_foreign_t*)a[0].u.ptr)->ptr;
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int fmt = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, fmt, &pfd);
    HGLRC ctx = fn_wglCreateContext(hdc);
    fn_wglMakeCurrent(hdc, ctx);
    load_modern_gl();
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ((ls_foreign_t*)v.u.ptr)->ptr = ctx;
    return v;
}

static ls_value_t bi_gl_swap_buffers(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1) { ls_error(L, "gl-swap-buffers: need hwnd"); return ls_nil_v(); }
    HWND hwnd = (HWND)((ls_foreign_t*)a[0].u.ptr)->ptr;
    HDC hdc = GetDC(hwnd);
    SwapBuffers(hdc);
    ReleaseDC(hwnd, hdc);
    return ls_nil_v();
}

static ls_value_t bi_gl_destroy_context(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n >= 1 && a[0].tag == LS_T_FOREIGN) {
        HGLRC ctx = (HGLRC)((ls_foreign_t*)a[0].u.ptr)->ptr;
        fn_wglMakeCurrent(NULL, NULL);
        fn_wglDeleteContext(ctx);
    }
    return ls_nil_v();
}

/* ---- Macros for quick GL wrappers ---- */
#define GL_WRAP_0(name, glfn) \
    static ls_value_t bi_##name(ls_state_t *L, int n, ls_value_t *a) { \
        (void)L;(void)n;(void)a; if(glfn) glfn(); return ls_nil_v(); }
#define GL_WRAP_1I(name, glfn) \
    static ls_value_t bi_##name(ls_state_t *L, int n, ls_value_t *a) { \
        (void)L; if(n<1) return ls_nil_v(); if(glfn) glfn((GLenum)a[0].u.fixnum); return ls_nil_v(); }
#define GL_WRAP_1F(name, glfn) \
    static ls_value_t bi_##name(ls_state_t *L, int n, ls_value_t *a) { \
        (void)L; if(n<1) return ls_nil_v(); \
        double v0 = a[0].tag==LS_T_FLONUM?a[0].u.flonum:(double)a[0].u.fixnum; \
        if(glfn) glfn((GLfloat)v0); return ls_nil_v(); }
#define NUMF(a) (a.tag==LS_T_FLONUM?a.u.flonum:(double)a.u.fixnum)

static ls_value_t bi_gl_clear_color(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(n<4||!fn_glClearColor) return ls_nil_v();
    fn_glClearColor((GLclampf)NUMF(a[0]),(GLclampf)NUMF(a[1]),(GLclampf)NUMF(a[2]),(GLclampf)NUMF(a[3]));
    return ls_nil_v();
}
static ls_value_t bi_gl_clear(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(!fn_glClear) return ls_nil_v();
    GLbitfield mask = n>=1 ? (GLbitfield)a[0].u.fixnum : GL_COLOR_BUFFER_BIT;
    fn_glClear(mask); return ls_nil_v();
}
static ls_value_t bi_gl_begin(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glBegin&&n>=1) fn_glBegin((GLenum)a[0].u.fixnum); return ls_nil_v(); }
GL_WRAP_0(gl_end, fn_glEnd)
static ls_value_t bi_gl_vertex2f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glVertex2f&&n>=2) fn_glVertex2f((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1])); return ls_nil_v(); }
static ls_value_t bi_gl_vertex3f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glVertex3f&&n>=3) fn_glVertex3f((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2])); return ls_nil_v(); }
static ls_value_t bi_gl_color3f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glColor3f&&n>=3) fn_glColor3f((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2])); return ls_nil_v(); }
static ls_value_t bi_gl_color4f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glColor4f&&n>=4) fn_glColor4f((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2]),(GLfloat)NUMF(a[3])); return ls_nil_v(); }
static ls_value_t bi_gl_normal3f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glNormal3f&&n>=3) fn_glNormal3f((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2])); return ls_nil_v(); }
static ls_value_t bi_gl_texcoord2f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glTexCoord2f&&n>=2) fn_glTexCoord2f((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1])); return ls_nil_v(); }
static ls_value_t bi_gl_viewport(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glViewport&&n>=4) fn_glViewport((GLint)a[0].u.fixnum,(GLint)a[1].u.fixnum,(GLsizei)a[2].u.fixnum,(GLsizei)a[3].u.fixnum); return ls_nil_v(); }
GL_WRAP_1I(gl_matrix_mode, fn_glMatrixMode)
GL_WRAP_0(gl_load_identity, fn_glLoadIdentity)
static ls_value_t bi_gl_ortho(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glOrtho&&n>=6) fn_glOrtho(NUMF(a[0]),NUMF(a[1]),NUMF(a[2]),NUMF(a[3]),NUMF(a[4]),NUMF(a[5])); return ls_nil_v(); }
static ls_value_t bi_gl_perspective(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(n<4||!fn_glFrustum) return ls_nil_v();
    double fovy=NUMF(a[0]), aspect=NUMF(a[1]), znear=NUMF(a[2]), zfar=NUMF(a[3]);
    double f = 1.0 / tan(fovy * 3.14159265358979323846 / 360.0);
    double ne_far = znear - zfar;
    GLfloat m[16]; memset(m,0,sizeof m);
    m[0]=(GLfloat)(f/aspect); m[5]=(GLfloat)f;
    m[10]=(GLfloat)((zfar+znear)/ne_far); m[11]=-1.0f;
    m[14]=(GLfloat)(2.0*zfar*znear/ne_far);
    fn_glMultMatrixf(m);
    return ls_nil_v();
}
static ls_value_t bi_gl_translatef(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glTranslatef&&n>=3) fn_glTranslatef((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2])); return ls_nil_v(); }
static ls_value_t bi_gl_rotatef(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glRotatef&&n>=4) fn_glRotatef((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2]),(GLfloat)NUMF(a[3])); return ls_nil_v(); }
static ls_value_t bi_gl_scalef(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glScalef&&n>=3) fn_glScalef((GLfloat)NUMF(a[0]),(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2])); return ls_nil_v(); }
GL_WRAP_0(gl_push_matrix, fn_glPushMatrix)
GL_WRAP_0(gl_pop_matrix, fn_glPopMatrix)
GL_WRAP_1I(gl_enable, fn_glEnable)
GL_WRAP_1I(gl_disable, fn_glDisable)
static ls_value_t bi_gl_blend_func(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glBlendFunc&&n>=2) fn_glBlendFunc((GLenum)a[0].u.fixnum,(GLenum)a[1].u.fixnum); return ls_nil_v(); }
GL_WRAP_0(gl_flush, fn_glFlush)
GL_WRAP_0(gl_finish, fn_glFinish)
static ls_value_t bi_gl_get_error(ls_state_t *L, int n, ls_value_t *a) { (void)L;(void)n;(void)a; return fn_glGetError ? ls_make_fixnum(fn_glGetError()) : ls_make_fixnum(0); }
GL_WRAP_1F(gl_line_width, fn_glLineWidth)
GL_WRAP_1F(gl_point_size, fn_glPointSize)

/* Texture ops */
static ls_value_t bi_gl_gen_texture(ls_state_t *L, int n, ls_value_t *a) {
    (void)n;(void)a; GLuint tex=0; if(fn_glGenTextures) fn_glGenTextures(1,&tex);
    return ls_make_fixnum(tex);
}
static ls_value_t bi_gl_bind_texture(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(fn_glBindTexture&&n>=2) fn_glBindTexture((GLenum)a[0].u.fixnum,(GLuint)a[1].u.fixnum); return ls_nil_v();
}
static ls_value_t bi_gl_tex_parameteri(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(fn_glTexParameteri&&n>=3) fn_glTexParameteri((GLenum)a[0].u.fixnum,(GLenum)a[1].u.fixnum,(GLint)a[2].u.fixnum); return ls_nil_v();
}

/* Shader ops */
static ls_value_t bi_gl_create_shader(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(!fn_glCreateShader||n<1) return ls_make_fixnum(0);
    return ls_make_fixnum(fn_glCreateShader((GLenum)a[0].u.fixnum));
}
static ls_value_t bi_gl_shader_source(ls_state_t *L, int n, ls_value_t *a) {
    if(!fn_glShaderSource||n<2) return ls_nil_v();
    ls_string_t *src = ls_string_p(a[1]); if(!src) return ls_nil_v();
    const GLchar *s = src->chars;
    GLint len = (GLint)src->len;
    fn_glShaderSource((GLuint)a[0].u.fixnum, 1, &s, &len);
    return ls_nil_v();
}
static ls_value_t bi_gl_compile_shader(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(fn_glCompileShader&&n>=1) fn_glCompileShader((GLuint)a[0].u.fixnum);
    if(fn_glGetShaderiv&&n>=1) {
        GLint ok=0; fn_glGetShaderiv((GLuint)a[0].u.fixnum, GL_COMPILE_STATUS, &ok);
        if(!ok && fn_glGetShaderInfoLog) {
            char log[512]; fn_glGetShaderInfoLog((GLuint)a[0].u.fixnum,512,NULL,log);
            ls_error(L, "GLSL compile error: %s", log);
        }
    }
    return ls_nil_v();
}
static ls_value_t bi_gl_create_program(ls_state_t *L, int n, ls_value_t *a) { (void)L;(void)n;(void)a; return fn_glCreateProgram?ls_make_fixnum(fn_glCreateProgram()):ls_make_fixnum(0); }
static ls_value_t bi_gl_attach_shader(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glAttachShader&&n>=2) fn_glAttachShader((GLuint)a[0].u.fixnum,(GLuint)a[1].u.fixnum); return ls_nil_v(); }
static ls_value_t bi_gl_link_program(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; if(fn_glLinkProgram&&n>=1) fn_glLinkProgram((GLuint)a[0].u.fixnum);
    if(fn_glGetProgramiv&&n>=1) {
        GLint ok=0; fn_glGetProgramiv((GLuint)a[0].u.fixnum, GL_LINK_STATUS, &ok);
        if(!ok && fn_glGetProgramInfoLog) {
            char log[512]; fn_glGetProgramInfoLog((GLuint)a[0].u.fixnum,512,NULL,log);
            ls_error(L, "GLSL link error: %s", log);
        }
    }
    return ls_nil_v();
}
static ls_value_t bi_gl_use_program(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glUseProgram&&n>=1) fn_glUseProgram((GLuint)a[0].u.fixnum); return ls_nil_v(); }
static ls_value_t bi_gl_get_uniform_location(ls_state_t *L, int n, ls_value_t *a) {
    if(!fn_glGetUniformLocation||n<2) return ls_make_fixnum(-1);
    ls_string_t *s = ls_string_p(a[1]); if(!s) return ls_make_fixnum(-1);
    return ls_make_fixnum(fn_glGetUniformLocation((GLuint)a[0].u.fixnum, s->chars));
}
static ls_value_t bi_gl_uniform1f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glUniform1f&&n>=2) fn_glUniform1f((GLint)a[0].u.fixnum,(GLfloat)NUMF(a[1])); return ls_nil_v(); }
static ls_value_t bi_gl_uniform3f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glUniform3f&&n>=4) fn_glUniform3f((GLint)a[0].u.fixnum,(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2]),(GLfloat)NUMF(a[3])); return ls_nil_v(); }
static ls_value_t bi_gl_uniform4f(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glUniform4f&&n>=5) fn_glUniform4f((GLint)a[0].u.fixnum,(GLfloat)NUMF(a[1]),(GLfloat)NUMF(a[2]),(GLfloat)NUMF(a[3]),(GLfloat)NUMF(a[4])); return ls_nil_v(); }

/* VBO / VAO */
static ls_value_t bi_gl_gen_buffer(ls_state_t *L, int n, ls_value_t *a) { (void)L;(void)n;(void)a; GLuint b=0; if(fn_glGenBuffers)fn_glGenBuffers(1,&b); return ls_make_fixnum(b); }
static ls_value_t bi_gl_bind_buffer(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glBindBuffer&&n>=2) fn_glBindBuffer((GLenum)a[0].u.fixnum,(GLuint)a[1].u.fixnum); return ls_nil_v(); }
static ls_value_t bi_gl_gen_vertex_array(ls_state_t *L, int n, ls_value_t *a) { (void)L;(void)n;(void)a; GLuint v=0; if(fn_glGenVertexArrays)fn_glGenVertexArrays(1,&v); return ls_make_fixnum(v); }
static ls_value_t bi_gl_bind_vertex_array(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glBindVertexArray&&n>=1) fn_glBindVertexArray((GLuint)a[0].u.fixnum); return ls_nil_v(); }
static ls_value_t bi_gl_draw_arrays(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glDrawArrays&&n>=3) fn_glDrawArrays((GLenum)a[0].u.fixnum,(GLint)a[1].u.fixnum,(GLsizei)a[2].u.fixnum); return ls_nil_v(); }
static ls_value_t bi_gl_draw_elements(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glDrawElements&&n>=4) fn_glDrawElements((GLenum)a[0].u.fixnum,(GLsizei)a[1].u.fixnum,(GLenum)a[2].u.fixnum,(const void*)(uintptr_t)a[3].u.fixnum); return ls_nil_v(); }
static ls_value_t bi_gl_enable_vertex_attrib(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glEnableVertexAttribArray&&n>=1) fn_glEnableVertexAttribArray((GLuint)a[0].u.fixnum); return ls_nil_v(); }

static ls_value_t bi_gl_delete_shader(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glDeleteShader&&n>=1) fn_glDeleteShader((GLuint)a[0].u.fixnum); return ls_nil_v(); }
static ls_value_t bi_gl_delete_program(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(fn_glDeleteProgram&&n>=1) fn_glDeleteProgram((GLuint)a[0].u.fixnum); return ls_nil_v(); }

#else /* Linux */
/* TODO: GLX-based loader */
static ls_value_t bi_gl_create_context(ls_state_t *L, int n, ls_value_t *a) { (void)L;(void)n;(void)a; ls_error(L,"GL context: Linux not yet implemented"); return ls_nil_v(); }
#endif /* _WIN32 */

/* ---- GL constant exports ---- */
static void def_gl_const(ls_state_t *L, const char *name, int64_t val) {
    ls_value_t s = ls_intern(L, "LITESRPENT-GL", name);
    ls_symbol_t *sym = (ls_symbol_t*)s.u.ptr;
    sym->value = ls_make_fixnum(val);
    sym->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_CONSTANT;
}

void ls_init_gl(ls_state_t *L) {
    ls_ensure_package(L, "LITESRPENT-GL");
#ifdef _WIN32
#define GLDEF(n,fn,mi,ma) ls_defun(L,"LITESRPENT-GL",n,fn,mi,ma)
    GLDEF("GL-CREATE-CONTEXT",bi_gl_create_context,1,1);
    GLDEF("GL-DESTROY-CONTEXT",bi_gl_destroy_context,1,1);
    GLDEF("GL-SWAP-BUFFERS",bi_gl_swap_buffers,1,1);
    GLDEF("GL-CLEAR-COLOR",bi_gl_clear_color,4,4);
    GLDEF("GL-CLEAR",bi_gl_clear,0,1);
    GLDEF("GL-BEGIN",bi_gl_begin,1,1);
    GLDEF("GL-END",bi_gl_end,0,0);
    GLDEF("GL-VERTEX2F",bi_gl_vertex2f,2,2);
    GLDEF("GL-VERTEX3F",bi_gl_vertex3f,3,3);
    GLDEF("GL-COLOR3F",bi_gl_color3f,3,3);
    GLDEF("GL-COLOR4F",bi_gl_color4f,4,4);
    GLDEF("GL-NORMAL3F",bi_gl_normal3f,3,3);
    GLDEF("GL-TEXCOORD2F",bi_gl_texcoord2f,2,2);
    GLDEF("GL-VIEWPORT",bi_gl_viewport,4,4);
    GLDEF("GL-MATRIX-MODE",bi_gl_matrix_mode,1,1);
    GLDEF("GL-LOAD-IDENTITY",bi_gl_load_identity,0,0);
    GLDEF("GL-ORTHO",bi_gl_ortho,6,6);
    GLDEF("GL-PERSPECTIVE",bi_gl_perspective,4,4);
    GLDEF("GL-TRANSLATEF",bi_gl_translatef,3,3);
    GLDEF("GL-ROTATEF",bi_gl_rotatef,4,4);
    GLDEF("GL-SCALEF",bi_gl_scalef,3,3);
    GLDEF("GL-PUSH-MATRIX",bi_gl_push_matrix,0,0);
    GLDEF("GL-POP-MATRIX",bi_gl_pop_matrix,0,0);
    GLDEF("GL-ENABLE",bi_gl_enable,1,1);
    GLDEF("GL-DISABLE",bi_gl_disable,1,1);
    GLDEF("GL-BLEND-FUNC",bi_gl_blend_func,2,2);
    GLDEF("GL-FLUSH",bi_gl_flush,0,0);
    GLDEF("GL-FINISH",bi_gl_finish,0,0);
    GLDEF("GL-GET-ERROR",bi_gl_get_error,0,0);
    GLDEF("GL-LINE-WIDTH",bi_gl_line_width,1,1);
    GLDEF("GL-POINT-SIZE",bi_gl_point_size,1,1);
    GLDEF("GL-GEN-TEXTURE",bi_gl_gen_texture,0,0);
    GLDEF("GL-BIND-TEXTURE",bi_gl_bind_texture,2,2);
    GLDEF("GL-TEX-PARAMETERI",bi_gl_tex_parameteri,3,3);
    GLDEF("GL-CREATE-SHADER",bi_gl_create_shader,1,1);
    GLDEF("GL-SHADER-SOURCE",bi_gl_shader_source,2,2);
    GLDEF("GL-COMPILE-SHADER",bi_gl_compile_shader,1,1);
    GLDEF("GL-CREATE-PROGRAM",bi_gl_create_program,0,0);
    GLDEF("GL-ATTACH-SHADER",bi_gl_attach_shader,2,2);
    GLDEF("GL-LINK-PROGRAM",bi_gl_link_program,1,1);
    GLDEF("GL-USE-PROGRAM",bi_gl_use_program,1,1);
    GLDEF("GL-GET-UNIFORM-LOCATION",bi_gl_get_uniform_location,2,2);
    GLDEF("GL-UNIFORM1F",bi_gl_uniform1f,2,2);
    GLDEF("GL-UNIFORM3F",bi_gl_uniform3f,4,4);
    GLDEF("GL-UNIFORM4F",bi_gl_uniform4f,5,5);
    GLDEF("GL-GEN-BUFFER",bi_gl_gen_buffer,0,0);
    GLDEF("GL-BIND-BUFFER",bi_gl_bind_buffer,2,2);
    GLDEF("GL-GEN-VERTEX-ARRAY",bi_gl_gen_vertex_array,0,0);
    GLDEF("GL-BIND-VERTEX-ARRAY",bi_gl_bind_vertex_array,1,1);
    GLDEF("GL-DRAW-ARRAYS",bi_gl_draw_arrays,3,3);
    GLDEF("GL-DRAW-ELEMENTS",bi_gl_draw_elements,4,4);
    GLDEF("GL-ENABLE-VERTEX-ATTRIB",bi_gl_enable_vertex_attrib,1,1);
    GLDEF("GL-DELETE-SHADER",bi_gl_delete_shader,1,1);
    GLDEF("GL-DELETE-PROGRAM",bi_gl_delete_program,1,1);
#undef GLDEF
#endif
    /* GL constants */
    def_gl_const(L, "GL-COLOR-BUFFER-BIT", GL_COLOR_BUFFER_BIT);
    def_gl_const(L, "GL-DEPTH-BUFFER-BIT", GL_DEPTH_BUFFER_BIT);
    def_gl_const(L, "GL-TRIANGLES", GL_TRIANGLES);
    def_gl_const(L, "GL-TRIANGLE-STRIP", GL_TRIANGLE_STRIP);
    def_gl_const(L, "GL-QUADS", GL_QUADS);
    def_gl_const(L, "GL-LINES", GL_LINES);
    def_gl_const(L, "GL-POINTS", GL_POINTS);
    def_gl_const(L, "GL-POLYGON", GL_POLYGON);
    def_gl_const(L, "GL-DEPTH-TEST", GL_DEPTH_TEST);
    def_gl_const(L, "GL-BLEND", GL_BLEND);
    def_gl_const(L, "GL-TEXTURE-2D", GL_TEXTURE_2D);
    def_gl_const(L, "GL-MODELVIEW", GL_MODELVIEW);
    def_gl_const(L, "GL-PROJECTION", GL_PROJECTION);
    def_gl_const(L, "GL-VERTEX-SHADER", GL_VERTEX_SHADER);
    def_gl_const(L, "GL-FRAGMENT-SHADER", GL_FRAGMENT_SHADER);
    def_gl_const(L, "GL-ARRAY-BUFFER", GL_ARRAY_BUFFER);
    def_gl_const(L, "GL-ELEMENT-ARRAY-BUFFER", GL_ELEMENT_ARRAY_BUFFER);
    def_gl_const(L, "GL-STATIC-DRAW", GL_STATIC_DRAW);
    def_gl_const(L, "GL-DYNAMIC-DRAW", GL_DYNAMIC_DRAW);
    def_gl_const(L, "GL-FLOAT", GL_FLOAT);
    def_gl_const(L, "GL-UNSIGNED-INT", GL_UNSIGNED_INT);
    def_gl_const(L, "GL-SRC-ALPHA", GL_SRC_ALPHA);
    def_gl_const(L, "GL-ONE-MINUS-SRC-ALPHA", GL_ONE_MINUS_SRC_ALPHA);
    def_gl_const(L, "GL-LINEAR", GL_LINEAR);
    def_gl_const(L, "GL-NEAREST", GL_NEAREST);
    def_gl_const(L, "GL-REPEAT", GL_REPEAT);
    def_gl_const(L, "GL-CLAMP-TO-EDGE", GL_CLAMP_TO_EDGE);
    def_gl_const(L, "GL-TEXTURE-MAG-FILTER", GL_TEXTURE_MAG_FILTER);
    def_gl_const(L, "GL-TEXTURE-MIN-FILTER", GL_TEXTURE_MIN_FILTER);
}
