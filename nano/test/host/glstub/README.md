# The GL stub

`nano_glstub` (`docs/linux/IMPLEMENTATION.md` C.1, D.3): every EGL and GLES entry point
`nano-headless` calls, with no GPU and no window, so Nano's real render code runs on the host.
Golden screenshots need a real renderer (Mesa's llvmpipe) and come later; the stub makes boot,
input, launches and settings testable now.

Two parts:

- `glue.c`, hand-written: the calls whose results Nano reads back. An EGL display, two configs
  (with and without alpha), pbuffer surfaces of the requested size and contexts exist; contexts
  are current per thread; `eglGetError` is `EGL_SUCCESS`; `eglQueryString(EGL_EXTENSIONS)` is
  empty and `eglGetProcAddress` returns null for everything (Nano checks each pointer;
  `DrmHost.cpp` installs its own fence functions). Object names count up; shaders compile and
  programs link with empty logs; framebuffers are complete; `glGetIntegerv` reports plausible
  limits and the bindings, viewport and pack alignment last set; `glGetString` names a GLES 2.0
  "thimsy-glstub" with no extensions; `glReadPixels` fills exactly the requested rectangle with
  zeros (honouring `GL_PACK_ALIGNMENT`); `glDrawArrays` and `glDrawElements` (and the instanced
  forms) count draws, which `DrmHost.cpp` adds to every `present` event; buffers map to scratch
  memory; fences are signalled.
- The generic stubs, generated at link time by `gen.py` from `nm` of the objects being linked:
  every `gl*` and `egl*` symbol they leave undefined that the glue does not define gets a stub
  that returns 0 (`gl*`) or 1 (`egl*`: `EGL_TRUE`, a non-null handle) and logs its name once
  (`GLSTUB_QUIET=1` silences it). Nothing generated is committed, so the list follows Nano's code.

No GL or EGL header is used: each function is defined from its ABI, so the stub builds where those
headers are not installed. A generic stub is defined with an empty parameter list (in C that says
nothing about the parameters), which is safe because every GL and EGL return type is an integer or
a pointer, returned in the same register on x86-64 and AArch64.

Known limits: a generic `glGet*` with an output pointer this list does not cover leaves the
caller's variable unset (add it to `glue.c` when a run shows one: the stub logs its name);
`glIsEnabled` and the other generic queries return 0.

Builds: `make -C nano/test/host check-headless-0` compiles the glue, runs `gen.py` on a test
program's object and links them (`tests/glstub_test.cpp`); the `headless` target does the same for
`nano-headless`.
