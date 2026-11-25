/*
 * Copyright (C) 2026 GammaOS
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

// Bidirectional text for display. Self-contained (no NanoMenu dependency) so
// drastic-nano's overlay renderer can share it with gammaos-nano's glyph
// renderer (NanoMenuRender.cpp), which runs it on every drawn string.

#ifndef GAMMAOS_NANO_BIDI_H
#define GAMMAOS_NANO_BIDI_H

#include <string>

namespace android {

// True when the first strong-direction codepoint of the UTF-8 string is an
// Arabic/Hebrew letter, i.e. the string reads right-to-left as a whole. Used
// for caret placement and field alignment decisions (digits and punctuation
// are direction-neutral and skipped).
bool nanoTextIsRtl(const char* s);

// True when the string contains ANY RTL letter (not just first-strong). The
// OSK preview uses this to avoid the caret-split path for a value that is
// LTR-first but has an embedded Arabic/Hebrew run: splitting at the caret and
// bidi-transforming each half independently would garble that run.
bool nanoTextHasRtl(const char* s);

// Logical order -> visual order for the glyph renderer: Arabic contextual
// shaping (presentation forms + lam-alef ligatures, ZWJ/ZWNJ aware), RTL run
// reversal with embedded Latin/digit runs kept left-to-right, paired-bracket
// mirroring inside RTL runs, and stripping of zero-width joiner/direction
// codepoints (no bundled font has glyphs for them, so they would render as
// '?' boxes). Returns the input unchanged when it contains neither RTL nor
// zero-width codepoints.
std::string nanoBidiVisual(const std::string& s);

} // namespace android

#endif // GAMMAOS_NANO_BIDI_H
