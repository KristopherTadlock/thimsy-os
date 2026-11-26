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

// The GL stub as nano-headless gets it: this program is compiled, gen.py writes the generic stubs
// from its undefined GL and EGL symbols minus the glue's, and the three link. The prototypes below
// are Khronos' (no GL headers are installed here); Nano calls the same functions through the real
// headers.

#include <stdint.h>
#include <string.h>

#include <string>
#include <thread>

#include <gtest/gtest.h>

extern "C" {
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;

// Hand-written in glue.c.
void* eglGetDisplay(void* native);
EGLBoolean eglInitialize(void* dpy, EGLint* major, EGLint* minor);
EGLBoolean eglChooseConfig(void* dpy, const EGLint* attribs, void** configs, EGLint size, EGLint* num);
void* eglCreatePbufferSurface(void* dpy, void* config, const EGLint* attribs);
EGLBoolean eglQuerySurface(void* dpy, void* surface, EGLint attr, EGLint* value);
void* eglCreateContext(void* dpy, void* config, void* share, const EGLint* attribs);
EGLBoolean eglMakeCurrent(void* dpy, void* draw, void* read, void* ctx);
void* eglGetCurrentContext(void);
EGLint eglGetError(void);
const char* eglQueryString(void* dpy, EGLint name);
void (*eglGetProcAddress(const char* name))(void);
const unsigned char* glGetString(GLenum name);
void glGenTextures(GLsizei n, GLuint* names);
void glGenFramebuffers(GLsizei n, GLuint* names);
void glBindFramebuffer(GLenum target, GLuint fb);
void glGetIntegerv(GLenum pname, GLint* params);
GLenum glCheckFramebufferStatus(GLenum target);
GLuint glCreateShader(GLenum type);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels);
void glPixelStorei(GLenum pname, GLint param);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
unsigned long glstub_draw_count(void);

// Generic: gen.py writes these.
void glClear(unsigned int mask);
void glEnable(GLenum cap);
void glUniform4f(GLint location, GLfloat a, GLfloat b, GLfloat c, GLfloat d);
GLboolean glIsEnabled(GLenum cap);
GLenum glGetError(void);
EGLBoolean eglSwapBuffers(void* dpy, void* surface);
EGLBoolean eglSwapInterval(void* dpy, EGLint interval);

extern const char* const glstub_generic_names[];
extern const unsigned glstub_generic_count;
}

namespace {

TEST(GlStub, EglBringUp) {
    void* dpy = eglGetDisplay(nullptr);
    ASSERT_TRUE(dpy != nullptr);
    EGLint major = 0, minor = 0;
    EXPECT_EQ(eglInitialize(dpy, &major, &minor), 1u);
    EXPECT_EQ(major, 1);
    const EGLint attribs[] = {0x3040, 0x4, 0x3024, 8, 0x3021, 0, 0x3038};
    void* config = nullptr;
    EGLint num = 0;
    EXPECT_EQ(eglChooseConfig(dpy, attribs, &config, 1, &num), 1u);
    EXPECT_EQ(num, 1);
    ASSERT_TRUE(config != nullptr);
    const EGLint pbuf[] = {0x3057, 16, 0x3056, 9, 0x3038};
    void* surf = eglCreatePbufferSurface(dpy, config, pbuf);
    ASSERT_TRUE(surf != nullptr);
    EGLint w = 0, h = 0;
    EXPECT_EQ(eglQuerySurface(dpy, surf, 0x3057, &w), 1u);
    EXPECT_EQ(eglQuerySurface(dpy, surf, 0x3056, &h), 1u);
    EXPECT_EQ(w, 16);
    EXPECT_EQ(h, 9);
    const EGLint ctxAttribs[] = {0x3098, 2, 0x3038};
    void* ctx = eglCreateContext(dpy, config, nullptr, ctxAttribs);
    ASSERT_TRUE(ctx != nullptr);
    EXPECT_EQ(eglMakeCurrent(dpy, surf, surf, ctx), 1u);
    EXPECT_EQ(eglGetCurrentContext(), ctx);
    void* other = nullptr;
    std::thread([&] { other = eglGetCurrentContext(); }).join();
    EXPECT_TRUE(other == nullptr);  // current per thread
    EXPECT_EQ(eglGetError(), 0x3000);
    EXPECT_STREQ(eglQueryString(dpy, 0x3055), "");
    EXPECT_TRUE(eglGetProcAddress("eglCreateImageKHR") == nullptr);
    EXPECT_EQ(eglSwapBuffers(dpy, surf), 1u);      // generic EGL: EGL_TRUE
    EXPECT_EQ(eglSwapInterval(dpy, 1), 1u);
}

TEST(GlStub, ObjectsShadersAndFramebuffers) {
    GLuint tex[3] = {};
    glGenTextures(3, tex);
    EXPECT_NE(tex[0], 0u);
    EXPECT_NE(tex[0], tex[1]);
    EXPECT_NE(tex[1], tex[2]);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    EXPECT_NE(fbo, 0u);
    glBindFramebuffer(0x8D40, fbo);
    GLint bound = -1;
    glGetIntegerv(0x8CA6, &bound);
    EXPECT_EQ((GLuint)bound, fbo);
    EXPECT_EQ(glCheckFramebufferStatus(0x8D40), 0x8CD5u);
    GLuint sh = glCreateShader(0x8B31);
    EXPECT_NE(sh, 0u);
    GLint ok = 0;
    glGetShaderiv(sh, 0x8B81, &ok);
    EXPECT_EQ(ok, 1);
    glGetProgramiv(sh, 0x8B82, &ok);
    EXPECT_EQ(ok, 1);
    glViewport(1, 2, 30, 40);
    GLint vp[4] = {};
    glGetIntegerv(0x0BA2, vp);
    EXPECT_EQ(vp[2], 30);
    EXPECT_EQ(vp[3], 40);
    GLint unknown = 77;
    glGetIntegerv(0x1234, &unknown);
    EXPECT_EQ(unknown, 0);
    EXPECT_STREQ(reinterpret_cast<const char*>(glGetString(0x1F02)), "OpenGL ES 2.0 thimsy-glstub");
}

TEST(GlStub, ReadPixelsZeroesTheWholeRequest) {
    unsigned char px[3 * 2 * 4 + 8];
    memset(px, 0xAB, sizeof(px));
    glReadPixels(0, 0, 3, 2, 0x1908, 0x1401, px);
    for (size_t i = 0; i < 24; i++) EXPECT_EQ(px[i], 0);
    EXPECT_EQ(px[24], 0xAB);  // and nothing past it
    // RGB rows padded to the pack alignment: 3 px * 3 B = 9, padded to 12; the last row unpadded.
    glPixelStorei(0x0D05, 4);
    memset(px, 0xAB, sizeof(px));
    glReadPixels(0, 0, 3, 2, 0x1907, 0x1401, px);
    for (size_t i = 0; i < 21; i++) EXPECT_EQ(px[i], 0);
    EXPECT_EQ(px[21], 0xAB);
}

TEST(GlStub, GenericStubsAndDrawCount) {
    const unsigned long before = glstub_draw_count();
    glClear(0x4000);
    glEnable(0x0BE2);
    glUniform4f(0, 1.0f, 2.0f, 3.0f, 4.0f);
    EXPECT_EQ(glIsEnabled(0x0BE2), 0);  // generic GL: 0
    EXPECT_EQ(glGetError(), 0u);
    glDrawArrays(4, 0, 3);
    glDrawArrays(4, 0, 3);
    EXPECT_EQ(glstub_draw_count(), before + 2);
}

TEST(GlStub, GeneratedExactlyTheMissingOnes) {
    std::string names;
    for (unsigned i = 0; i < glstub_generic_count; i++) names += std::string(glstub_generic_names[i]) + " ";
    EXPECT_EQ(names, "eglSwapBuffers eglSwapInterval glClear glEnable glGetError glIsEnabled glUniform4f ");
    EXPECT_TRUE(glstub_generic_names[glstub_generic_count] == nullptr);
}

}  // namespace
