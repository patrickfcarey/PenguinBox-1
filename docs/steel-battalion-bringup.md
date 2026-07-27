# Steel Battalion — emulation bring-up (agent handoff)

**Branch:** `steel-battalion` (off `vr/mvp`). **Goal:** get **Steel Battalion**
(original Xbox) to reach *gameplay* in this xemu fork, then wire the real
controller. (VR-cockpit integration is tracked separately in the private
framework docs; this doc is purely about getting the game to *run*.)

**Git identity (H-1):** commit as `Patrick Carey <patrickfcarey@gmail.com>`,
setting BOTH author and committer, and **no `Co-Authored-By` trailer**. Never
"Claude Code" in either field.

---

## Current status (2026-07-23) — **GAMEPLAY REACHED, VERIFIED IN-GAME**

Verified on the rig the same evening: intro → menus → character signup →
opening cutscene → **in-cockpit**, with a fully clean log (zero nv2a
lines; the levels==0 diagnostic never fired). Both root-cause fixes hold.
Launch recipe: `~/sb-launch.sh` on the rig (PRIME env forced — the VULKAN
display path needs GL+VK on the same NVIDIA device even flat; SDL x11 for
XWayland capture; `~/xemu-assets/sb-verify.toml`). Next: the in-cockpit
controls pass (SB startup ritual, ISS-B12 hotkey cleanup), then Phase 3.
Tracking: **ISS-B11** (FIXED/verified), **ISS-B12** (hotkeys), ISS-B09.

| Commit | What |
|---|---|
| `a5eaaa6732` | squash-port of upstream **PR #1803** — SBC device emulation (input) |
| `e72aa3f6b8` | texture.c:294 assert → log-and-survive diagnostic |
| `3b7360159f` | squash-port of upstream **PR #2514** — DMA via mem callbacks (the real texture fix) |

## Findings (2026-07-23 four-agent run; full detail in ISS-B11)

- **The texture assert was NOT a decode bug and NOT a clampable config.**
  The decode chain register→bitfield→`texture.c` is bit-faithful (L1, static
  proof: every input is a masked unsigned field; `max_mipmap_level+1` cannot
  wrap). Upstream #1238 (L2): real NV2A **faults** on `MIPMAP_LEVELS==0`
  (abaire, HW-verified); the offending word `0xFF000000` is the
  `MmAllocateContiguousMemory` **uninitialized fill** read as a texture
  format — xemu gave guest DMA (async `NtReadFile`) a direct RAM pointer
  without syncing the GPU surface first. The naive `levels=MAX(levels,1)`
  clamp was tested upstream and **failed**. Fix = **PR #2514** (open
  upstream; test builds reach the in-cockpit view; residual freeze reports
  under review; 2026-07-24 re-check: no freeze-with-fix reports in the
  PR thread, but upstream success is scoped to the FIRST ~3 LEVELS —
  a crash after level 2-3 is reported; expect a next blocker deep in
  the campaign and capture logs when it fires).
- **Menu input is gated on the SBC device** (L3). The game polls only the
  SBC XID (`bType 0x80`, xboxdevwiki) and ignores gamepad XIDs. With PR
  #1803's device + keyboard placeholder, menus and character creation work
  (xemu#2252 — which then crashed at exactly our texture assert; closed dup
  of #1238). **Texture fix + SBC device are a pair**; nobody has ever
  reached SB gameplay.
- **PR #1803 port** (L4): zero conflicts (merge-tree-verified squash, tree
  `63fc7a13`); durable device emulation (26/32-byte XID reports, no-hub
  topology) vs placeholder input mapping (52-key scancode map, mouse→aiming
  lever). Placeholder → real multi-device binding is Phase 3.
- **PR #2514 port**: zero overlap with our tree (we don't touch
  gl/surface.c, vk/surface.c, system/physmem.c; no drift since the PR's
  base, which is in our history). `#ifdef XBOX`-gated physmem change.

## What remains — the rig loop

1. **Restore rig SSH** — user runs `~/.claude/runs/rig-authorize-key.sh`
   (re-authorizes the dev box's `id_ed25519`; verifies BatchMode login).
2. **Sync the rig** to this branch. The rig cannot fetch origin (ISS-B09) —
   push from the dev box directly into the rig checkout over SSH
   (`git push ssh://$RIG_SSH/~/xemu_vr steel-battalion:steel-battalion`,
   then on the rig `git checkout steel-battalion`), or scp a bundle. Run
   `tools/vr/rig-sync-check.sh` from the dev box; reconcile per AGENTS §5.
3. **Build on the rig** (`./build.sh`); confirm `build/qemu-system-i386`
   mtime is newer than the sync (stale-binary rule). `config_spec.yml`
   changed → codegen regenerates automatically; sanity:
   `keyboard_sbc_scancode_map` present in the generated config header.
4. **Verify, flat first (H-2 live-session check before ANY launch;
   `vr.enable = false`; debug BIOS per H-5):**
   - Launch with the SB ISO. **An SB ISO must be on the rig — locate or ask
     the user** (none was confirmed present when this was written).
   - Bind port 1 driver = Steel Battalion Controller, input = keyboard
     (Input menu; `input.bindings.port1_driver = 'usb-steel-battalion'`).
   - Expect: intro FMV → menus **navigable via keyboard** → character
     creation → "start new game" → **no assert** → in-cockpit view.
   - The `nv2a: texture N MIPMAP_LEVELS==0` diagnostic must stay **silent**;
     if it fires, a surface-sync hole remains — capture the logged words
     (`fmt==0xFF000000` = stale fill) and re-open the investigation.
   - Watch for the upstream-reported freezes/deadlocks (#2514 review); note
     hotkey weirdness is expected (ISS-B12: F5–F12 double-bound).
   - Off-state check: a Duke-bound, `vr.enable=false` run of another title
     must behave as before (off-state discipline, AGENTS §5).
5. **Phase 3 (after gameplay verified):** real SBC input binding
   (placeholder → multi-device), ISS-B12 hotkey gating, then the VR cockpit
   work (private docs).

## Key files

- `hw/xbox/nv2a/pgraph/texture.c` — the diagnostic (ex-assert), mip math
- `hw/xbox/nv2a/pgraph/{gl,vk}/surface.c`, `system/physmem.c` — the #2514 port
- `hw/xbox/xid-steel-battalion.c`, `hw/xbox/xid.h` — the SBC XID device
- `ui/xemu-input.c` — SBC input population (placeholder mapping lives here)
- `config_spec.yml` — `keyboard_sbc_scancode_map` (input:) + our `vr:` section
- Local refs kept: `pr-1803` = `37f9bf78f6`, `pr-2514` = `03f2bf4e0e`
