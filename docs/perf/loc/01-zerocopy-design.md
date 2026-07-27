# Zero-copy surface-as-texture (`XEMU_SURF2TEX_ZEROCOPY`) — design & review notes

**Target:** the last measured LoC wall — ~700 exact-path surface→texture copies
(≈700 renderpasses) per heavy frame, correlating 1:1 with the ~190 ms windows
(fast frames with identical draw volume: 18 renderpasses, 31–53 ms).

**Idea:** when the exact-match compat test passes, don't copy the surface image
into a texture image — create a `VkImageView` onto the surface's own `VkImage`
and sample it directly (what the GL backend has always done). Kills the
per-cycle copy+barrier cluster; redraw-rebind cycles need **no** re-copy at all
(the view sees live content); renderpass merging is no longer broken by
interleaved nondraw copies.

Opt-in: `XEMU_SURF2TEX_ZEROCOPY=1`, kill-switch `XEMU_NO_SURF2TEX_ZEROCOPY`.
Composes with (but does not require) `XEMU_SURF2TEX_EXT` / `XEMU_ALIAS_SURFACES`.

## The seven problems and their answers

1. **Layout choreography.** Surfaces historically rest in
   `COLOR_ATTACHMENT_OPTIMAL`; sampling needs `SHADER_READ_ONLY_OPTIMAL`; the
   cached renderpasses hardcode ATTACHMENT initial/final layouts (draw.c:429).
   → Add **per-surface `image_layout` tracking** plus one helper,
   `pgraph_vk_surface_transition()`, used by *every* surface-image transition
   site (download, upload, S2T copies, display composite). New invariant: the
   only code allowed to *leave* a surface in a non-ATTACHMENT resting state is
   the zero-copy bind (leaves SHADER_READ); `begin_render_pass()` restores the
   two bound targets to ATTACHMENT before `vkCmdBeginRenderPass` — that
   restore is also the write-after-read barrier for the sampling.
2. **Feedback loops** (sampling the current render target is illegal in VK).
   → Zero-copy is refused when `surface == r->color_binding || zeta_binding`;
   those binds take the historical copy path. A borrowed cache node whose
   surface *became* the current target is rebuilt as a copy-node for that bind.
3. **Lifetime.** The node's view borrows the surface's image.
   → Node owns view+sampler only (`borrowed` flag guards
   `texture_cache_release_node_resources` — never destroys the image/allocation);
   a global `surface_generation`, bumped in `invalidate_surface()`, plus the
   stored `borrow_image` handle invalidate borrowed nodes on cache hit (view
   rebuild is cheap — no copies involved); in-flight GPU reads are protected by
   the existing narrow-fence `last_use_submit` gate, stamped at every
   zero-copy bind exactly as the copy path stamps it.
4. **View format rules.** Surface images lack `MUTABLE_FORMAT_BIT`.
   → Zero-copy only when `tex vkf.vk_format == surface->host_fmt.vk_format`
   (the dominant ping-pong class); component swizzles come from the texture's
   `component_map` on the *view* (always legal). Same-bpp-different-format
   binds keep the copy path. Color surfaces only in v8 (the zeta exact path is
   a format *conversion*, not a reinterpret — stays on its copy path).
5. **Redraw semantics.** GPU executes commands in order: a sampling draw
   recorded before a later redraw reads pre-redraw content — identical
   semantics to the copy the historical path would have taken at bind time.
   The ATTACHMENT-restore transition doubles as the hazard barrier.
6. **Alias interplay.** Zero-copy only engages on the *exact-compat* path;
   the freshness-ruled EXT paths are untouched.
7. **The `check_textures_dirty` hole.** A draw can reuse cached bindings
   without running `create_texture`, after a redraw restored the surface to
   ATTACHMENT — the sampled image would be in the wrong layout.
   → `pgraph_vk_bind_textures()` ensures SHADER_READ for all *bound borrowed*
   nodes before its early-out (no-op unless a transition is actually needed;
   when needed, the renderpass was already broken by the redraw itself).

## Files touched

| file | change |
|---|---|
| `renderer.h` | `SurfaceBinding.image_layout`; `PGRAPHVkState.surface_generation`; `TextureBinding.{borrowed, borrow_gen, borrow_image}`; helper proto |
| `surface.c` | field init at surface create; `pgraph_vk_surface_transition()`; `surface_generation++` in `invalidate_surface`; download/upload sites converted to the helper |
| `texture.c` | copy-path surface-side transitions converted; zero-copy gate + miss/hit paths; borrowed-release guard; `bind_textures` layout-ensure |
| `draw.c` | `begin_render_pass` restores bound targets to ATTACHMENT |
| `display.c` | composite/present surface transitions converted to the helper |

## Validation plan

1. Snapshot benchmark: EXT+ALIAS+ZEROCOPY vs EXT+ALIAS — expect the ~190 ms
   copy windows to collapse (target: total work < 33.4 ms → vblank-locked 30).
2. 0 VK validation errors (the layout machinery is exactly what validation
   watches).
3. SB gates: default flags byte-identical; all three flags ON — expect
   engaged-and-benign like rounds 1–2.
4. Owner visuals (shadows + cockpit) — required before any default flip.

## v8 verdict (2026-07-27, snapshot benchmark) — honest result

**Stability: PASSED.** One boot crash found in validation (the transition
helper's pair table lacked `SHADER_READ→TRANSFER_SRC` and friends — added)
then a full storm-scene run: **0 VK validation errors, no crash**, all three
flags on.

**Engagement: PARTIAL, and it names the real blocker.** Zero-copy fully
serves some heavy frames (`mspf=57, s2t_copies=0` — every bind borrowed). But
the ~190 ms frames still run ~700 copies: those are **feedback-pattern
frames** — the game samples the surface *while it is still the bound render
target* (legal on the Xbox's unified memory; illegal to sample in Vulkan), so
the deliberate `surface == color_binding` exclusion routes them to the copy
path. Two theory corrections from the data: (a) the renderpass count (~656)
persists even with zero copies — the passes are genuine per-block target
switches, not copy-splits; (b) median mspf 54.1 / p90 209 ≈ round 2 —
**v8 is performance-neutral on this benchmark.**

**v9 direction (not implemented):** feedback-tolerant sampling — the surface
in `VK_IMAGE_LAYOUT_GENERAL` with self-dependency barriers between the block
draws (or `VK_EXT_attachment_feedback_loop_layout` where available), so the
feedback frames can borrow too. That is the remaining path to collapsing the
190 ms windows; the layout-tracking machinery v8 built is its prerequisite
either way.
