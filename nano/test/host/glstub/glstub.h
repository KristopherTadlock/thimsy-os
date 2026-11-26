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

/* The GL stub's internals (nano/test/host/glstub/README.md). Shared by the hand-written glue
 * (glue.c) and the file gen.py generates; Nano never includes it, and it includes no GL or EGL
 * header: every function is defined from its ABI alone, so the stub builds where those headers are
 * not installed. */
#ifndef NANO_GLSTUB_H
#define NANO_GLSTUB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Logs "glstub: NAME called (generic stub)" to stderr the first time NAME runs, unless GLSTUB_QUIET
 * is set in the environment. *seen is the function's own flag. */
void glstub_note_generic(const char* name, int* seen);

/* How many draw calls (glDrawArrays, glDrawElements and their instanced forms) ran. DrmHost adds it
 * to each {"ev":"present"}. */
unsigned long glstub_draw_count(void);

#ifdef __cplusplus
}
#endif

/* A generic stub: a function of any signature that does nothing and returns 0 (gl*) or 1 (egl*:
 * EGL_TRUE for the EGLBoolean calls, and a non-null handle for the rest). It is defined with an
 * empty parameter list, which in C says nothing about the parameters, so a caller compiled against
 * the real prototype passes its arguments as usual and the stub ignores them. Every GL and EGL
 * return type is an integer or a pointer, returned in the same register as an unsigned long on the
 * x86-64 and AArch64 ABIs. */
#define GLSTUB_GENERIC_GL(name)                         \
    static int glstub_seen_##name;                      \
    unsigned long name() {                              \
        glstub_note_generic(#name, &glstub_seen_##name); \
        return 0;                                       \
    }
#define GLSTUB_GENERIC_EGL(name)                        \
    static int glstub_seen_##name;                      \
    unsigned long name() {                              \
        glstub_note_generic(#name, &glstub_seen_##name); \
        return 1;                                       \
    }

#endif /* NANO_GLSTUB_H */
