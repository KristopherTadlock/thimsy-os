/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* The GL stub's hand-written part (README.md): the EGL and GLES calls whose results Nano reads
 * back, so a zero would send it down a failure path or leave its variables unset. An EGL display,
 * configs, pbuffer surfaces and contexts exist and can be made current; object names count up;
 * shaders compile and programs link; framebuffers are complete; glReadPixels returns zeros;
 * glGetError returns 0 and eglGetError EGL_SUCCESS; the glGet* queries write plausible limits.
 * Everything else is a generic stub that gen.py writes from the link's undefined symbols.
 *
 * No GL or EGL header is included (they are not installed everywhere this builds): the types below
 * are the ABI's, and each signature is written to match Khronos' prototype. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glstub.h"

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef unsigned int GLbitfield;
typedef void* GLsync;

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* EGLNativeDisplayType;
typedef void* EGLNativeWindowType;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;
typedef void (*eglMustCastToProperFunctionPointerType)(void);

#define UNUSED(x) (void)(x)

/* ---- bookkeeping ------------------------------------------------------------------------------ */

static unsigned long sNextName = 1;     /* texture, buffer, framebuffer, ... names */
static unsigned long sNextObject = 1;   /* shader and program names */
static unsigned long sNextLocation = 0; /* uniform locations */
static unsigned long sDraws = 0;

static unsigned long next(unsigned long* counter) { return __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED); }

void glstub_note_generic(const char* name, int* seen) {
    if (__atomic_exchange_n(seen, 1, __ATOMIC_RELAXED)) return;
    const char* quiet = getenv("GLSTUB_QUIET");
    if (quiet && *quiet && strcmp(quiet, "0") != 0) return;
    fprintf(stderr, "glstub: %s called (generic stub)\n", name);
}

unsigned long glstub_draw_count(void) { return __atomic_load_n(&sDraws, __ATOMIC_RELAXED); }

/* GL state the queries report (per thread: one context per thread in Nano). */
static __thread GLuint tFramebuffer, tTexture, tProgram, tArrayBuffer, tElementBuffer, tRenderbuffer;
static __thread GLint tViewport[4];
static __thread GLint tPackAlignment = 4, tUnpackAlignment = 4;
static __thread GLuint tActiveTexture = 0x84C0; /* GL_TEXTURE0 */
static __thread EGLDisplay tDisplay;
static __thread EGLSurface tDraw, tRead;
static __thread EGLContext tContext;
static __thread EGLint tEglError = 0x3000; /* EGL_SUCCESS */

/* ---- EGL -------------------------------------------------------------------------------------- */

#define STUB_DISPLAY ((EGLDisplay)(uintptr_t)0xd15)
#define STUB_CONFIG_OPAQUE ((EGLConfig)(uintptr_t)0xc01)
#define STUB_CONFIG_ALPHA ((EGLConfig)(uintptr_t)0xc02)

#define MAX_SURFACES 256
static struct { EGLint w, h; } sSurfaces[MAX_SURFACES];
static unsigned long sNextSurface = 0;
static unsigned long sNextContext = 0;

static EGLSurface makeSurface(EGLint w, EGLint h) {
    const unsigned long i = next(&sNextSurface) % MAX_SURFACES;
    sSurfaces[i].w = w;
    sSurfaces[i].h = h;
    return (EGLSurface)(uintptr_t)(0x5000 + i * 16);
}

static int surfaceIndex(EGLSurface s) {
    const uintptr_t v = (uintptr_t)s;
    if (v < 0x5000 || (v - 0x5000) % 16 != 0 || (v - 0x5000) / 16 >= MAX_SURFACES) return -1;
    return (int)((v - 0x5000) / 16);
}

static EGLint attrib(const EGLint* list, EGLint name, EGLint def) {
    for (; list && list[0] != 0x3038 /* EGL_NONE */; list += 2) {
        if (list[0] == name) return list[1];
    }
    return def;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType native) {
    UNUSED(native);
    return STUB_DISPLAY;
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native, const EGLAttrib* attribs) {
    UNUSED(platform), UNUSED(native), UNUSED(attribs);
    return STUB_DISPLAY;
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void* native, const EGLint* attribs) {
    UNUSED(platform), UNUSED(native), UNUSED(attribs);
    return STUB_DISPLAY;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    UNUSED(dpy);
    if (major) *major = 1;
    if (minor) *minor = 4;
    return 1;
}

EGLint eglGetError(void) {
    const EGLint e = tEglError;
    tEglError = 0x3000;
    return e;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attribs, EGLConfig* configs, EGLint size, EGLint* num) {
    UNUSED(dpy);
    const EGLConfig c = attrib(attribs, 0x3021 /* EGL_ALPHA_SIZE */, 0) > 0 ? STUB_CONFIG_ALPHA : STUB_CONFIG_OPAQUE;
    EGLint n = 1;
    if (configs) {
        n = size > 0 ? 1 : 0;
        if (n) configs[0] = c;
    }
    if (num) *num = n;
    return 1;
}

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint size, EGLint* num) {
    UNUSED(dpy);
    EGLint n = 2;
    if (configs) {
        n = size < 2 ? (size > 0 ? size : 0) : 2;
        if (n > 0) configs[0] = STUB_CONFIG_OPAQUE;
        if (n > 1) configs[1] = STUB_CONFIG_ALPHA;
    }
    if (num) *num = n;
    return 1;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attr, EGLint* value) {
    UNUSED(dpy);
    if (!value) return 0;
    switch (attr) {
        case 0x3024: case 0x3023: case 0x3022: *value = 8; break;             /* RED, GREEN, BLUE */
        case 0x3021: *value = config == STUB_CONFIG_ALPHA ? 8 : 0; break;    /* ALPHA */
        case 0x3020: *value = config == STUB_CONFIG_ALPHA ? 32 : 24; break;  /* BUFFER_SIZE */
        case 0x3028: *value = config == STUB_CONFIG_ALPHA ? 2 : 1; break;    /* CONFIG_ID */
        case 0x302E: *value = 1; break;                                      /* NATIVE_VISUAL_ID: RGBA_8888 */
        case 0x3033: *value = 0x1 | 0x4; break;                              /* SURFACE_TYPE: PBUFFER | WINDOW */
        case 0x3040: case 0x3042: *value = 0x4 | 0x40; break;                /* RENDERABLE_TYPE, CONFORMANT: ES2 | ES3 */
        case 0x302C: *value = 16384; break;                                  /* MAX_PBUFFER_WIDTH */
        case 0x302A: *value = 16384; break;                                  /* MAX_PBUFFER_HEIGHT */
        default: *value = 0; break;
    }
    return 1;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attribs) {
    UNUSED(dpy), UNUSED(config);
    return makeSurface(attrib(attribs, 0x3057 /* EGL_WIDTH */, 0), attrib(attribs, 0x3056 /* EGL_HEIGHT */, 0));
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint* attribs) {
    UNUSED(dpy), UNUSED(config), UNUSED(win), UNUSED(attribs);
    return makeSurface(0, 0);
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attr, EGLint* value) {
    UNUSED(dpy);
    const int i = surfaceIndex(surface);
    if (!value || i < 0) {
        tEglError = 0x300D; /* EGL_BAD_SURFACE */
        return 0;
    }
    switch (attr) {
        case 0x3057: *value = sSurfaces[i].w; break; /* EGL_WIDTH */
        case 0x3056: *value = sSurfaces[i].h; break; /* EGL_HEIGHT */
        default: *value = 0; break;
    }
    return 1;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint* attribs) {
    UNUSED(dpy), UNUSED(config), UNUSED(share), UNUSED(attribs);
    return (EGLContext)(uintptr_t)(0x7000 + next(&sNextContext) * 16);
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    tDisplay = ctx ? dpy : 0;
    tDraw = ctx ? draw : 0;
    tRead = ctx ? read : 0;
    tContext = ctx;
    return 1;
}

EGLContext eglGetCurrentContext(void) { return tContext; }
EGLDisplay eglGetCurrentDisplay(void) { return tDisplay; }
EGLSurface eglGetCurrentSurface(EGLint which) { return which == 0x305A /* EGL_READ */ ? tRead : tDraw; }

const char* eglQueryString(EGLDisplay dpy, EGLint name) {
    UNUSED(dpy);
    switch (name) {
        case 0x3053: return "Thimsy";               /* EGL_VENDOR */
        case 0x3054: return "1.4 thimsy-glstub";    /* EGL_VERSION */
        case 0x3055: return "";                     /* EGL_EXTENSIONS: none */
        case 0x308D: return "OpenGL_ES";            /* EGL_CLIENT_APIS */
        default: tEglError = 0x300C; return NULL;   /* EGL_BAD_PARAMETER */
    }
}

/* No extension functions: Nano checks each pointer it resolves. DrmHost installs its own fence
 * functions. */
eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    UNUSED(name);
    return NULL;
}

/* ---- GL: strings and queries ------------------------------------------------------------------ */

const GLubyte* glGetString(GLenum name) {
    switch (name) {
        case 0x1F00: return (const GLubyte*)"Thimsy";                       /* GL_VENDOR */
        case 0x1F01: return (const GLubyte*)"thimsy-glstub";                /* GL_RENDERER */
        case 0x1F02: return (const GLubyte*)"OpenGL ES 2.0 thimsy-glstub";  /* GL_VERSION */
        case 0x8B8C: return (const GLubyte*)"OpenGL ES GLSL ES 1.00";       /* GL_SHADING_LANGUAGE_VERSION */
        case 0x1F03: return (const GLubyte*)"";                             /* GL_EXTENSIONS: none */
        default: return NULL;
    }
}

/* Writes the value(s) of pname; returns how many it wrote, 0 for an unknown name. */
static int intQuery(GLenum pname, GLint* p) {
    switch (pname) {
        case 0x0BA2: memcpy(p, tViewport, sizeof(tViewport)); return 4;   /* GL_VIEWPORT */
        case 0x0D3A: p[0] = p[1] = 16384; return 2;                        /* GL_MAX_VIEWPORT_DIMS */
        case 0x0D33: case 0x84E8: case 0x851C: p[0] = 16384; return 1;    /* MAX_TEXTURE, RENDERBUFFER, CUBE_MAP size */
        case 0x8CA6: p[0] = (GLint)tFramebuffer; return 1;                /* GL_FRAMEBUFFER_BINDING */
        case 0x8CA7: p[0] = (GLint)tRenderbuffer; return 1;               /* GL_RENDERBUFFER_BINDING */
        case 0x8069: p[0] = (GLint)tTexture; return 1;                    /* GL_TEXTURE_BINDING_2D */
        case 0x8B8D: p[0] = (GLint)tProgram; return 1;                    /* GL_CURRENT_PROGRAM */
        case 0x8894: p[0] = (GLint)tArrayBuffer; return 1;                /* GL_ARRAY_BUFFER_BINDING */
        case 0x8895: p[0] = (GLint)tElementBuffer; return 1;              /* GL_ELEMENT_ARRAY_BUFFER_BINDING */
        case 0x84E0: p[0] = (GLint)tActiveTexture; return 1;              /* GL_ACTIVE_TEXTURE */
        case 0x0D05: p[0] = tPackAlignment; return 1;                     /* GL_PACK_ALIGNMENT */
        case 0x0CF5: p[0] = tUnpackAlignment; return 1;                   /* GL_UNPACK_ALIGNMENT */
        case 0x8872: case 0x8B4C: p[0] = 16; return 1;                    /* MAX_(VERTEX_)TEXTURE_IMAGE_UNITS */
        case 0x8B4D: p[0] = 32; return 1;                                 /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
        case 0x8869: p[0] = 16; return 1;                                 /* GL_MAX_VERTEX_ATTRIBS */
        case 0x8DFB: case 0x8DFD: p[0] = 256; return 1;                   /* MAX_VERTEX/FRAGMENT_UNIFORM_VECTORS */
        case 0x8DFC: p[0] = 15; return 1;                                 /* GL_MAX_VARYING_VECTORS */
        case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: p[0] = 8; return 1;  /* RED/GREEN/BLUE/ALPHA_BITS */
        case 0x0D56: case 0x0D57: p[0] = 0; return 1;                     /* DEPTH_BITS, STENCIL_BITS */
        case 0x8B9A: p[0] = 0x1401; return 1;                             /* IMPLEMENTATION_COLOR_READ_TYPE: UNSIGNED_BYTE */
        case 0x8B9B: p[0] = 0x1908; return 1;                             /* IMPLEMENTATION_COLOR_READ_FORMAT: RGBA */
        case 0x821B: p[0] = 2; return 1;                                  /* GL_MAJOR_VERSION */
        case 0x821C: p[0] = 0; return 1;                                  /* GL_MINOR_VERSION */
        case 0x821D: p[0] = 0; return 1;                                  /* GL_NUM_EXTENSIONS */
        default: return 0;
    }
}

void glGetIntegerv(GLenum pname, GLint* params) {
    if (params && intQuery(pname, params) == 0) params[0] = 0;
}

void glGetInteger64v(GLenum pname, GLint64* params) {
    GLint v[4] = {0, 0, 0, 0};
    const int n = intQuery(pname, v);
    if (!params) return;
    for (int i = 0; i < (n > 0 ? n : 1); i++) params[i] = v[i];
}

void glGetFloatv(GLenum pname, GLfloat* params) {
    if (!params) return;
    if (pname == 0x846D || pname == 0x846E) { /* ALIASED_POINT_SIZE_RANGE, ALIASED_LINE_WIDTH_RANGE */
        params[0] = 1.0f;
        params[1] = 64.0f;
        return;
    }
    GLint v[4] = {0, 0, 0, 0};
    const int n = intQuery(pname, v);
    for (int i = 0; i < (n > 0 ? n : 1); i++) params[i] = (GLfloat)v[i];
}

void glGetBooleanv(GLenum pname, GLboolean* params) {
    if (!params) return;
    GLint v[4] = {0, 0, 0, 0};
    const int n = intQuery(pname, v);
    for (int i = 0; i < (n > 0 ? n : 1); i++) params[i] = v[i] != 0;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    UNUSED(shader);
    if (params) params[0] = pname == 0x8B81 /* GL_COMPILE_STATUS */ ? 1 : 0;
}

void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    UNUSED(program);
    if (params) params[0] = (pname == 0x8B82 /* LINK_STATUS */ || pname == 0x8B83 /* VALIDATE_STATUS */) ? 1 : 0;
}

static void emptyLog(GLsizei bufSize, GLsizei* length, GLchar* text) {
    if (length) *length = 0;
    if (text && bufSize > 0) text[0] = '\0';
}

void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    UNUSED(shader);
    emptyLog(bufSize, length, infoLog);
}

void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
    UNUSED(program);
    emptyLog(bufSize, length, infoLog);
}

void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source) {
    UNUSED(shader);
    emptyLog(bufSize, length, source);
}

void glGetShaderPrecisionFormat(GLenum shaderType, GLenum precisionType, GLint* range, GLint* precision) {
    UNUSED(shaderType), UNUSED(precisionType);
    if (range) range[0] = range[1] = 127;
    if (precision) *precision = 23;
}

/* The glGet*v calls with one output value: 0. */
void glGetTexParameteriv(GLenum t, GLenum p, GLint* v) { UNUSED(t), UNUSED(p); if (v) v[0] = 0; }
void glGetTexParameterfv(GLenum t, GLenum p, GLfloat* v) { UNUSED(t), UNUSED(p); if (v) v[0] = 0; }
void glGetBufferParameteriv(GLenum t, GLenum p, GLint* v) { UNUSED(t), UNUSED(p); if (v) v[0] = 0; }
void glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint* v) { UNUSED(t), UNUSED(p); if (v) v[0] = 0; }
void glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint* v) {
    UNUSED(t), UNUSED(a), UNUSED(p);
    if (v) v[0] = 0;
}
void glGetVertexAttribiv(GLuint i, GLenum p, GLint* v) { UNUSED(i), UNUSED(p); if (v) v[0] = 0; }
void glGetVertexAttribfv(GLuint i, GLenum p, GLfloat* v) { UNUSED(i), UNUSED(p); if (v) v[0] = 0; }
void glGetUniformiv(GLuint prog, GLint loc, GLint* v) { UNUSED(prog), UNUSED(loc); if (v) v[0] = 0; }
void glGetUniformfv(GLuint prog, GLint loc, GLfloat* v) { UNUSED(prog), UNUSED(loc); if (v) v[0] = 0; }
void glGetQueryiv(GLenum t, GLenum p, GLint* v) { UNUSED(t), UNUSED(p); if (v) v[0] = 0; }
void glGetQueryObjectuiv(GLuint id, GLenum p, GLuint* v) { UNUSED(id), UNUSED(p); if (v) v[0] = 0; }
void glGetQueryObjectuivEXT(GLuint id, GLenum p, GLuint* v) { UNUSED(id), UNUSED(p); if (v) v[0] = 0; }
void glGetQueryObjectui64vEXT(GLuint id, GLenum p, GLuint64* v) { UNUSED(id), UNUSED(p); if (v) v[0] = 0; }

/* ---- GL: objects ------------------------------------------------------------------------------ */

static void genNames(GLsizei n, GLuint* names) {
    for (GLsizei i = 0; names && i < n; i++) names[i] = (GLuint)next(&sNextName);
}

void glGenTextures(GLsizei n, GLuint* names) { genNames(n, names); }
void glGenBuffers(GLsizei n, GLuint* names) { genNames(n, names); }
void glGenFramebuffers(GLsizei n, GLuint* names) { genNames(n, names); }
void glGenRenderbuffers(GLsizei n, GLuint* names) { genNames(n, names); }
void glGenVertexArrays(GLsizei n, GLuint* names) { genNames(n, names); }
void glGenVertexArraysOES(GLsizei n, GLuint* names) { genNames(n, names); }
void glGenQueries(GLsizei n, GLuint* names) { genNames(n, names); }
void glGenQueriesEXT(GLsizei n, GLuint* names) { genNames(n, names); }

GLuint glCreateShader(GLenum type) {
    UNUSED(type);
    return (GLuint)next(&sNextObject);
}

GLuint glCreateProgram(void) { return (GLuint)next(&sNextObject); }

GLboolean glIsTexture(GLuint n) { return n != 0; }
GLboolean glIsBuffer(GLuint n) { return n != 0; }
GLboolean glIsFramebuffer(GLuint n) { return n != 0; }
GLboolean glIsRenderbuffer(GLuint n) { return n != 0; }
GLboolean glIsShader(GLuint n) { return n != 0; }
GLboolean glIsProgram(GLuint n) { return n != 0; }

GLint glGetUniformLocation(GLuint program, const GLchar* name) {
    UNUSED(program), UNUSED(name);
    return (GLint)(next(&sNextLocation) & 0x7fffffff);
}

GLint glGetAttribLocation(GLuint program, const GLchar* name) {
    UNUSED(program), UNUSED(name);
    return (GLint)(next(&sNextLocation) % 16);
}

GLenum glCheckFramebufferStatus(GLenum target) {
    UNUSED(target);
    return 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */
}

/* ---- GL: state the queries report ------------------------------------------------------------- */

void glBindFramebuffer(GLenum target, GLuint fb) {
    UNUSED(target);
    tFramebuffer = fb;
}

void glBindRenderbuffer(GLenum target, GLuint rb) {
    UNUSED(target);
    tRenderbuffer = rb;
}

void glBindTexture(GLenum target, GLuint tex) {
    if (target == 0x0DE1 /* GL_TEXTURE_2D */) tTexture = tex;
}

void glActiveTexture(GLenum unit) { tActiveTexture = unit; }

void glUseProgram(GLuint program) { tProgram = program; }

void glBindBuffer(GLenum target, GLuint buffer) {
    if (target == 0x8892 /* GL_ARRAY_BUFFER */) tArrayBuffer = buffer;
    if (target == 0x8893 /* GL_ELEMENT_ARRAY_BUFFER */) tElementBuffer = buffer;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    tViewport[0] = x;
    tViewport[1] = y;
    tViewport[2] = w;
    tViewport[3] = h;
}

void glPixelStorei(GLenum pname, GLint param) {
    if (pname == 0x0D05) tPackAlignment = param;   /* GL_PACK_ALIGNMENT */
    if (pname == 0x0CF5) tUnpackAlignment = param; /* GL_UNPACK_ALIGNMENT */
}

/* ---- GL: pixels, draws, buffers and syncs ----------------------------------------------------- */

static size_t componentsOf(GLenum format) {
    switch (format) {
        case 0x1908: case 0x80E1: return 4;  /* RGBA, BGRA_EXT */
        case 0x1907: return 3;               /* RGB */
        case 0x190A: case 0x8227: return 2;  /* LUMINANCE_ALPHA, RG */
        default: return 1;                   /* ALPHA, LUMINANCE, RED, ... */
    }
}

static size_t bytesOf(GLenum type) {
    switch (type) {
        case 0x1406: case 0x1405: case 0x1404: return 4;  /* FLOAT, UNSIGNED_INT, INT */
        case 0x1403: case 0x1402: case 0x8D61: return 2;  /* UNSIGNED_SHORT, SHORT, HALF_FLOAT_OES */
        default: return 1;
    }
}

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    UNUSED(x), UNUSED(y);
    if (!pixels || w <= 0 || h <= 0) return;
    size_t pixel = componentsOf(format) * bytesOf(type);
    /* Packed types hold a whole pixel in one value. */
    if (type == 0x8033 || type == 0x8034 || type == 0x8363) pixel = 2; /* 4444, 5551, 565 */
    const size_t align = tPackAlignment > 0 ? (size_t)tPackAlignment : 1;
    const size_t row = ((size_t)w * pixel + align - 1) / align * align;
    memset(pixels, 0, row * (size_t)(h - 1) + (size_t)w * pixel);
}

static void drew(void) { __atomic_add_fetch(&sDraws, 1, __ATOMIC_RELAXED); }

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    UNUSED(mode), UNUSED(first), UNUSED(count);
    drew();
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    UNUSED(mode), UNUSED(count), UNUSED(type), UNUSED(indices);
    drew();
}

void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    UNUSED(mode), UNUSED(first), UNUSED(count), UNUSED(instances);
    drew();
}

void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances) {
    UNUSED(mode), UNUSED(count), UNUSED(type), UNUSED(indices), UNUSED(instances);
    drew();
}

/* One scratch buffer for every thread (only the render thread maps buffers); kept until exit. */
static void* tMapped;
static size_t tMappedSize;

void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    UNUSED(target), UNUSED(offset), UNUSED(access);
    if (length <= 0) return NULL;
    if ((size_t)length > tMappedSize) {
        void* p = realloc(tMapped, (size_t)length);
        if (!p) return NULL;
        tMapped = p;
        tMappedSize = (size_t)length;
    }
    return tMapped;
}

GLboolean glUnmapBuffer(GLenum target) {
    UNUSED(target);
    return 1;
}

GLboolean glUnmapBufferOES(GLenum target) {
    UNUSED(target);
    return 1;
}

GLsync glFenceSync(GLenum condition, GLbitfield flags) {
    UNUSED(condition), UNUSED(flags);
    return (GLsync)(uintptr_t)(0x9000 + next(&sNextObject) * 16);
}

GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    UNUSED(sync), UNUSED(flags), UNUSED(timeout);
    return 0x911A; /* GL_ALREADY_SIGNALED */
}
