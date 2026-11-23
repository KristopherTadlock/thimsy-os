# Global shader extension plan

Date: 2026-11-08  
Status: Proposed design; shader and compositor changes are not implemented.

## 1. Objective and requirements

Extend Thimsy's shader infrastructure so a preset can declare whether its input
is the full display composition or a selected emulator's submitted surface.

The intended setup is:

- Emulators running inside RetroArch use a simple RetroArch shader.
- Standalone emulators use a Thimsy surface filter.
- Emulator APKs and emulator binaries remain unchanged.
- Standalone content is projected to 800 × 600, cropped by 40 projected pixels
  from each side, and displayed as 720 × 600 on the RG Cube's 720 × 720 screen.
- Preserve all 600 projected vertical pixels and center them with 60 pixels of
  space above and below.
- Minimize GPU work and bandwidth. Add no extra frame queue or presentation
  stage for the initial single-pass scaler.

This is an OS compositor feature. A shader declaration tells Thimsy where to
run the effect; Thimsy must implement the corresponding execution path.

## 2. Terminology

**Composition target:** The combined display image after Android has positioned,
scaled, and blended its visible layers. On the Cube this is normally 720 × 720.

**Surface target:** The selected emulator's submitted Android surface buffer,
together with its buffer crop, orientation, and content rectangle. Filtering
happens while drawing this layer into the composition target.

**Internal game framebuffer:** An image inside the emulator before it submits
its Android surface. The compositor cannot automatically access this image.

The keyword `surface` means the submitted emulator buffer. Android's display
output surface already contains the composition, so calling both of these
"output surface" would be ambiguous.

The solution accepts whatever resolution the emulator submits. If an emulator
has already scaled or blurred its internal image, that processing remains in
the submitted buffer.

## 3. Current implementation and insertion points

Android source checkout:
`/home/michaeltadlock/gammaos-src`.

The following locations were inspected while preparing this plan. Line numbers
are navigation hints for the current checkout and can move.

| Source under `frameworks/native/` | Relevant behavior |
| --- | --- |
| `libs/renderengine/skia/SkiaRenderEngine.cpp:118` | The global effect scratch surface uses the display output surface's dimensions. |
| `libs/renderengine/skia/SkiaRenderEngine.cpp:759` | Global effects redirect composition into an offscreen surface. |
| `libs/renderengine/skia/SkiaRenderEngine.cpp:806` | RenderEngine draws the supplied layers individually. |
| `libs/renderengine/skia/SkiaRenderEngine.cpp:996` | An individual layer's buffer is imported and its image shader is constructed. |
| `libs/renderengine/skia/SkiaRenderEngine.cpp:1031` | Existing texture transforms account for layer geometry and buffer coordinates. |
| `libs/renderengine/skia/SkiaRenderEngine.cpp:1210` | The global effect runs after layer composition. |
| `services/surfaceflinger/SurfaceFlinger.cpp:3502` and `:3837` | Enabling the current global shader forces client/GPU composition. |
| `services/surfaceflinger/CompositionEngine/src/OutputLayer.cpp:287` | Per-layer composition state and forcing of client composition are available. |
| `libs/renderengine/include/renderengine/LayerSettings.h` | Carries buffer, sampling, geometry, and transform information; equality is relevant to composition caching. |
| `libs/renderengine/skia/filters/GammaGLSLShaderChain.cpp:266` | The GLSL loader already extracts pragma metadata from shader source. |
| `libs/renderengine/skia/filters/GammaCustomShader.cpp:395` | The custom Slang-to-SkSL path extracts parameter pragmas. |
| `libs/renderengine/skia/filters/GammaVulkanShaderChain.cpp:303` | Vulkan shader loading interprets stage and other metadata pragmas. |

The existing global helpers accept source and destination composition surfaces.
Their public `apply()` functions are not already a direct per-layer sampling
interface.

## 4. Proposed shader declaration

The following syntax is proposed; it is not currently supported.

For a surface effect:

```glsl
#pragma gammaos_target surface
```

For the existing global behavior:

```glsl
#pragma gammaos_target composition
```

### Parsing rules

1. Read the declaration as Thimsy metadata before submitting source to the
   GLSL, Slang, or SkSL compilation path.
2. Remove the declaration from compiler input, retaining its newline for useful
   compiler line numbers.
3. Store an enum with the loaded preset, conceptually
   `ShaderTarget::Composition` or `ShaderTarget::Surface`.
4. An absent declaration means `composition`.
5. Reject unknown target values and contradictory declarations with a clear
   preset-load error.
6. Apply one target to the entire preset/chain. Passes without a declaration
   inherit the preset target; declarations in other passes must agree.
7. Read metadata before stage extraction so a declaration in shared shader
   source is retained as policy. Resolve supported includes consistently with
   the existing loader.
8. Parse and validate at load/reload time, then cache the result.

An optional future preset field, such as
`gammaos_target = "surface"` in a `.glslp` or `.slangp`, could make routing
visible without opening shader source. If supported, source and preset
declarations must agree. This field is not necessary for the first delivery.

This changes Thimsy's loading contract. It does not require changing the
underlying shader languages or their compilers. Compatibility of this custom
pragma with other shader hosts should be checked separately.

## 5. Routing and preset lifecycle

The chosen target must be available to SurfaceFlinger **before composition
strategy selection**, not first discovered inside the post-composition draw.

Use a prepared preset description containing:

- Target and execution capabilities.
- A compiled filter or prepared chain, with resource ownership appropriate to
  its rendering context.
- Parameter values and a generation/version identifier.
- Per-output configuration.
- Validation/error status.

Load source and prepare metadata outside the critical per-frame path. Compile
GPU objects on the appropriate render context without introducing per-frame
compilation. Publish validated configuration atomically to the composition
policy and renderer.

A target, profile, or preset change must request a new composition, invalidate
affected geometry/damage and cached composition requests, and retire old
resources safely. Do not expose an unprepared surface target to the composition
policy.

For the surface route, global scratch allocation and the global post-pass are
unnecessary unless a separate composition effect is also active. Audit existing
`persist.gammaos.shader.enable` checks: distinguish "some shader enabled" from
"a composition post-effect enabled." Keep composition forcing required by other
features, including BFI.

Initial scope: one selected preset with one target at a time. RetroArch's own
shader remains independent. Simultaneous OS surface and OS composition effects
can be added later with explicitly ordered, separate preset slots.

On loading failure, keep a previous valid configuration where possible. Otherwise
draw normally and report the error. Never apply a surface-only preset to the full
composition as an implicit fallback.

## 6. Selecting the game surface

A surface declaration chooses the stage, not which app or layer to transform.

Use a per-app profile associated with the launched emulator and its owning
identity. Resolve the game's render surface to a stable layer identifier while
that surface exists.

Profiles should cover:

- Whether the OS filter is enabled.
- Game surface selection.
- Content rectangle within the submitted buffer.
- Projection size and display placement.
- Aspect policy, sampling parameters, and shader preset.
- The display on which the profile applies.

Exclude the RetroArch packages used by this build. Exclude the launcher, Nano
menu, system UI, unrelated app windows, and emulator controls that are separate
layers.

A debug surface-name hint can assist setup. Ownership and stable layer identity
should drive selection rather than a substring match against all visible layer
names. Refresh selection after surface recreation, activity transitions, and
resolution changes.

When game content and controls share one buffer, the compositor has no separate
control layer to preserve. Configure a content rectangle where possible; anything
inside that rectangle is part of the filtered input.

Respect protected-buffer and secure-display rules. Bypass unsupported buffers
through the normal rendering path rather than copying them into an incompatible
filter path.

## 7. Projection and crop geometry

### Cube profile

The source content rectangle can have any positive dimensions. Normalize it to
the following logical projection:

| Property | Value |
| --- | --- |
| Logical projected image | 800 × 600 |
| Placement on 720 × 720 display | Left = −40; top = 60 |
| Visible output rectangle | Left = 0; top = 60; width = 720; height = 600 |
| Horizontal projected crop | 40 pixels left and 40 pixels right |
| Vertical projected crop | None |
| Space above/below image | 60 pixels each |

The 800 × 600 projection is a coordinate mapping, not an allocated image.

### Direct sampling

For integer display pixel indices `x` and `y`, drawing only within
`x = 0..719` and `y = 60..659`, pixel-center coordinates in the logical
projection are:

```text
projected_x = x + 0.5 + 40
projected_y = y + 0.5 - 60

u = projected_x / 800
v = projected_y / 600
```

Map `(u, v)` into the configured source content rectangle, then apply the
sharp bilinear sampling-coordinate adjustment. Follow the renderer's existing
pixel-center convention and buffer crop/orientation transforms.

The visible region corresponds to the middle 90% of the content width and the
full content height. For a 640 × 480 input, the continuous horizontal boundaries
are 32 and 608 source pixels: 576 source pixels span the visible 720-pixel width.

For an input of `W × H`, the logical scales are `800/W` and `600/H`.
Non-4:3 content is stretched under this explicit projection policy. Offer
aspect-preserving profiles separately if desired.

### Content rectangles and transforms

Submitted buffers may include black bars, padding, or an already scaled game
image. Use the actual content rectangle rather than assuming that every buffer
pixel belongs to the game.

Compose the filter mapping with existing buffer crop and orientation transforms.
Account for allocation dimensions separately from the active content dimensions.
Do not rotate twice or apply an existing scale twice.

Update layer bounds, visible regions, occlusion, and damage **before culling and
composition decisions**. A game previously occupying 720 × 540 must be allowed
to draw the new 720 × 600 region. Changing only the shader's sampling coordinates
is insufficient.

Define clearing/background behavior for the exposed top and bottom regions so
they cannot retain stale pixels.

Apply this exact geometry to the Cube's internal display profile. External
displays need their own placement and dimensions; do not reuse 720 × 720
assumptions during hotplug or mirroring.

## 8. Sharp bilinear filter implementation

For the initial scaler, build a layer-local Skia runtime effect that:

1. Receives the imported image as a linearly filtered child shader.
2. Calculates the source position from the projection and content rectangle.
3. Adjusts coordinates for the selected sharp bilinear kernel.
4. Samples the image and returns its color through the existing layer pipeline.

Preserve existing handling of alpha, color spaces, tone mapping, dimming, and
layer blending. Use source-content clamping that prevents bars or adjacent
buffer pixels bleeding into the game image.

The expected efficient form adds coordinate arithmetic around a hardware
bilinear texture sample. Confirm the generated GPU shader and actual sampling
cost on the device.

"Sharp bilinear" needs a precise kernel definition and a reference image.
Some RetroArch sharp bilinear implementations derive an integer prescale using
the floor of the enlargement factor. At 1.25× that prescale can be one, making
the result ordinary bilinear. Choose the desired kernel explicitly rather than
assuming every preset bearing that name produces the same result.

For submitted content larger than 800 × 600, the operation includes minification.
Provide a deliberate minification policy. A single bilinear sample is cheap but
does not provide general antialiasing for substantial reductions. More elaborate
filtering should be an explicit quality/performance choice.

Keep geometric placement in the OS profile and expose consistent size/mapping
uniforms to the filter. This lets simple shader code focus on sampling without
requiring every preset to rediscover display layout.

## 9. Scope and backend capabilities

Accepting a pragma does not make every existing shader executable during a
single layer draw.

### First delivery

Support the requested single-pass scaler through a direct Skia runtime-effect
path compatible with the active RenderEngine backend.

Share metadata parsing and applicable shader-loading utilities with the existing
global infrastructure. Add a layer-filter construction interface rather than
calling a global `apply(dstSurface, srcSurface)` helper from inside the layer loop.

Initially accept `surface` only for shaders that the direct path can execute
faithfully. Report an explicit capability error for unsupported vertex behavior,
samplers, history textures, or other requirements. Avoid silently translating a
complex preset into a different effect.

Existing GLSL/Vulkan composition presets retain their current route. Support for
their syntax, stage semantics, and GPU resource integration in the direct surface
path must be implemented and validated before claiming surface compatibility.

### Later multipass support

A surface-targeted multipass preset can run against an isolated layer image and
then blend its result into the composition. This typically requires intermediate
textures, extra draws, and synchronization. Budget these costs separately.

A directive identifies the intended input; it is not a promise of zero-cost
execution. Full compatibility with arbitrary RetroArch presets is outside the
initial single-pass delivery.

### Current backend risks

The inspected Vulkan chain contains synchronous `submitAndWait()`/
`vkWaitForFences`, a `flushAndSubmit(GrSyncCpu::kYes)` path, and a possible
`readPixels()` fallback. The existing GLSL chain also flushes with CPU
synchronization before extracting the source texture.

Do not reuse these paths unchanged for the low-latency scaler. The initial path
must stay inside the existing GPU composition draw and synchronization model.

## 10. Performance and latency design

### Required properties for the initial scaler

- Use the existing imported emulator buffer.
- Draw directly into the existing client composition target.
- Allocate no separate 800 × 600 or full-screen shader input image.
- Introduce no screen capture, virtual display, CPU readback, or extra frame queue.
- Reuse existing acquire/release fence handling.
- Add no new CPU wait solely to extract or process the image.
- Cache shader compilation, profile resolution, and relevant metadata.
- Avoid file reads, property polling, allocations, and verbose logging per layer
  per frame.
- Filter only the selected game surface.

Force the selected game layer into client/GPU composition, preserving hardware
composition eligibility for other layers where the device supports the mixture.

The HWC may impose ordering or plane constraints that make additional layers
client-composed. Measure the actual composition strategy rather than assuming
that all other layers remain in hardware.

A game that previously used direct scanout will now require GPU composition.
Consequently this design can avoid an extra buffered frame while still increasing
GPU time, bandwidth, power, or missed presentation deadlines.

If ordinary vendor-controlled scaling is acceptable, HWC scaling and cropping
may be cheaper. An exact custom sharp bilinear kernel requires programmable
sampling, which is why the proposed route uses RenderEngine.

Do not claim zero added latency without device measurements. The design target
is no additional presentation stage and a small bounded increase in existing
composition work.

## 11. Configuration, cache correctness, and input

Keep shader input dimensions tied to the real submitted content and buffer.
Provide distinct values for content size, texture/allocation size, projection
size, and visible output geometry as needed.

The existing global GLSL/Vulkan paths can advertise synthetic source dimensions
to control CRT/LCD density. Do not accidentally inherit those dimensions as the
surface scaler's true sampling resolution.

Extend composition request equality/cache keys for all new draw-affecting state:
target, selected layer, profile/preset generation, parameters, content rectangle,
projection, and output transform. Profile or shader changes must redraw even when
the emulator's buffer has not changed.

Retire cached objects safely after surface recreation, output changes, or shader
switches. Preserve correct behavior during partial damage and buffer reuse.

A visual transform can change where touch input must land. For touchscreen games,
map touches through the inverse projection and existing layer/input transforms.
Check emulator controls and system gestures separately. Gamepad-only operation
does not require remapping controller input.

This input work does not require recompiling the emulator, but it may require
additional OS input/window integration beyond a RenderEngine filter.

## 12. Implementation sequence

### Phase 1: Metadata and policy

- Add shared target parsing and validation across loaders.
- Preserve composition as the default.
- Publish prepared preset metadata before composition strategy selection.
- Add per-app profiles and a RetroArch exclusion.
- Separate surface-effect enablement from composition-post-effect enablement.

### Phase 2: Direct Cube scaler

- Identify the selected emulator surface and content bounds.
- Carry filter/profile state into layer settings and composition cache keys.
- Update visibility, geometry, damage, and client-composition selection.
- Build the direct sharp bilinear runtime effect.
- Implement the 800 × 600 projection and horizontal crop.
- Preserve existing color, alpha, fence, and blending behavior.

### Phase 3: Lifecycle and interaction

- Handle resolution changes, surface recreation, pause/resume, and app switches.
- Define failures and ordinary-draw fallback behavior.
- Configure per-display behavior and hotplug handling.
- Implement or explicitly gate touchscreen support.

### Phase 4: Validation and distribution

- Validate geometry, sampling, cache invalidation, and app selection.
- Measure composition cost and latency on the physical RG Cube.
- Export the Android source changes into this project's maintained patch/build
  workflow.
- Build matching system/firmware artifacts and record source provenance.

### Phase 5: Optional expansion

- Add further standalone-emulator profiles.
- Extend backend/preset compatibility where needed.
- Add independently selected composition and surface effect slots.
- Evaluate multipass surface effects with explicit cost reporting.

## 13. Verification and acceptance criteria

These are proposed implementation checks; no firmware changes or hardware tests
were performed to create this document.

### Metadata and routing

- Absent target preserves existing composition behavior.
- Explicit targets select the expected stage.
- Conflicting/unknown declarations produce a useful error and ordinary rendering.
- Shared source/stage/include handling preserves the metadata.
- Unsupported surface presets are rejected without misrouting.
- Shader/profile switches invalidate cached output without needing a new game
  buffer.

### Geometry and sampling

Use a test image with a coordinate grid and uniquely marked edges, presented
through a normal Android game-like surface.

Verify:

- The logical projection is 800 × 600.
- Exactly 40 projected pixels are removed from the left and right.
- All projected vertical content is retained.
- Visible output is 720 × 600 at display position (0, 60).
- No border bleed, stale margins, or half-pixel displacement appears.
- Rotation, source crop, and surface resizing preserve the mapping.
- Arbitrary submitted sizes and configured letterboxed content rectangles work.
- The filter matches the selected sharp bilinear reference kernel.
- Minification follows the declared quality policy.

Check RetroArch with its own shader, Dolphin with both supported graphics APIs,
and another standalone emulator using different buffer dimensions. Inspect actual
submitted surfaces; internal resolution settings are not proof of buffer size.

### Layer lifecycle and interaction

Verify that launcher/system overlays remain correctly positioned, selected game
surfaces refresh after recreation, app switches disable the old profile, and
touch input matches the transformed image where supported.

Test pause/resume, lock/unlock, preset failure, and external-display attach/detach
without stale geometry or invalid resource reuse.

### Performance

Compare equivalent runs on the same hardware:

1. No OS effect.
2. Existing full-composition shader.
3. Proposed surface scaler.

Keep game, internal resolution, refresh rate, thermal state, and other effects
consistent. Record actual client/device composition decisions, GPU/composition
time, presentation misses, and frame-time percentiles using suitable Android
tracing.

Confirm absence of an additional buffer queue, CPU image readback, and shader-
introduced synchronous waits. At 60 Hz the total frame interval is approximately
16.67 ms; identify whether the extra GPU work causes existing deadlines to be
missed.

For input-to-display latency, use a suitable hardware/high-speed-camera method
with repeatable input and visual response. GPU timing or screenshots alone do
not establish end-to-end latency.

### Completion criteria

The first delivery is complete when the target directive routes correctly, the
Cube geometry and filter are verified, standalone emulator binaries remain
unchanged, RetroArch receives no second OS scaling pass, and measured composition
cost/latency are recorded with their practical limits.

## 14. Decisions to retain

- Use `#pragma gammaos_target surface|composition` as loader metadata.
- Default existing presets to `composition`.
- Target one selected game surface rather than every layer owned by the app.
- Keep the requested projection and content bounds in a per-app OS profile.
- Start with a direct single-pass scaler and explicit capability checks.
- Treat arbitrary multipass preset compatibility as a later feature.
- Preserve the exact horizontal crop: 40 projected pixels on each side.
- Access the emulator's submitted surface without requiring emulator changes.

