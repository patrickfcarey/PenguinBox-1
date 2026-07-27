# CLAUDE.md — project instructions for PenguinBox

This file is loaded automatically at the start of every session in this repo.
It is deliberately short; the depth lives in the docs it points to.

**PenguinBox** is our VR build of **xemu** (the original-Xbox emulator, itself a
QEMU fork). It is the Xbox core in the PenguinVR family, the sibling of the PS2
core (`pcsx2-VR`), the PS1 core (`duckstation_vr`), and the N64 core
(`mupen64plus-VR`). The paradigm, the hard rules, and the three-tier VR model
are shared across all of them; this repo is the Xbox-specific instance.

> **Branding note (2026-07-17):** the product is **PenguinBox**. The code still
> carries `xemu-VR` in file headers and log strings — a mechanical rebrand pass
> is pending (see AGENTS.md §5). Use **PenguinBox** in all new prose.

## ► FIRST: read [AGENTS.md](AGENTS.md)

**[AGENTS.md](AGENTS.md) is the operating manual for any agent working on this
project** — the hard rules, the decision trees, the reverse-engineering loop,
the tooling, and the build/test reality of the shared two-machine setup. **Read
it before doing substantive work.** Everything below is a pointer or a rule
important enough to state twice.

## Non-negotiable rules (full table in AGENTS.md §1)

- **Git identity (H-1):** commit as `Patrick Carey <patrickfcarey@gmail.com>`,
  setting **both** author AND committer, and **no `Co-Authored-By` trailer**:
  ```
  GIT_COMMITTER_NAME="Patrick Carey" GIT_COMMITTER_EMAIL="patrickfcarey@gmail.com" \
    git commit --author="Patrick Carey <patrickfcarey@gmail.com>" -m "..."
  ```
  Never "Claude Code" in either identity field. Same rule on both machines.
- **Live-session check before ANY VR/GPU action on the rig (H-2):** three
  emulators now share the rig and the one headset. Check all three:
  `pgrep -x pcsx2-qt`, `pgrep -x mupen64plus`, **`pgrep -f "[q]emu-system-i386"`**
  — xemu's binary name exceeds pgrep's 15-char `comm` limit, so `-x` silently
  never matches; use `-f`, and keep the `[q]` bracket when checking over ssh
  (a bare pattern matches the remote shell carrying it — 2026-07-23 field hit).
  A live session means someone may be in the headset:
  stop and ask. Never chain a launch behind the check in one command.
- **Force the VULKAN renderer (H-3).** `get_default_renderer()` prefers OPENGL,
  and a failed VK init **silently falls back** to it with only a notification.
  VR requires the Vulkan NV2A path — set `display.renderer = 'VULKAN'` and
  detect "renderer is not VK" **loudly** rather than half-initializing.
- **WiVRn stop order (H-9):** never stop `wivrn-server` while a headset client
  is connected (26.6.1 segfaults). Recovery order: quit the emulator → close
  the headset client → restart the server.
- **Never claim we invented the core VR tech (H-7).** World-locked screen +
  reprojection is standard OpenXR; head-look camera injection is the
  flat2VR/UEVR lineage. Our claim is EXECUTION + the per-game grind + cadence.
  **No "Steam/SteamOS" in the product name (H-8) — and this repo is PUBLIC:
  unannounced family/framework roadmap items live in the private framework
  repo and are never named, referenced, or hinted at here.**

## Doc map (the knowledge base)

| Need | Doc |
|---|---|
| Agent orientation, hard rules, decision trees | **[AGENTS.md](AGENTS.md)** |
| System architecture (front door) | [ARCHITECTURE.md](ARCHITECTURE.md) |
| How we reverse-engineer cameras (the RAM hunt) | [RESEARCH.md](RESEARCH.md) |
| Every tool + when to use it | [tools/vr/README.md](tools/vr/README.md) |
| Step-by-step rig procedures | [runbooks/](runbooks/README.md) |
| The MVP plan + increment log + deep reviews | [docs/vr/00-mvp-plan.md](docs/vr/00-mvp-plan.md) |
| The PCSX2→xemu port map (task-by-task) | [docs/vr/01-port-map.md](docs/vr/01-port-map.md) |
| Per-feature design docs | [docs/features/](docs/features/README.md) |
| Bugs & investigation state | [KNOWN_ISSUES.md](KNOWN_ISSUES.md) |
| What's in flight right now | [CURRENT_WORKING_LOG.md](CURRENT_WORKING_LOG.md) |
| Roadmap / vision | [FUTURE_STATE.md](FUTURE_STATE.md) |
| Games needing in-headset testing | [TEST_QUEUE.md](TEST_QUEUE.md) |
| Machine-local rig details (SSH etc.) | `.env.local` (git-ignored; template `.env.local.example`) |
| Backlog / forward ideas (Touch input, gestures) | [docs/vr/02-gesture-mapping-concept.md](docs/vr/02-gesture-mapping-concept.md) + 00-mvp-plan §10 |

The framework-level sources this fork is built from live in sibling repos:
`retro-vr-framework/docs/` (the integration spec + xemu feasibility verdict),
`pcsx2-VR/pcsx2/VR/` (the working Vulkan OpenXR implementation this port
mirrors), and `mupen64plus-VR/docs/vr/S0-rig-findings.md` (rig environment
facts + the GL-session-is-poison / Vulkan-compositor decision).

## Build/test reality (details in AGENTS.md §5)

Builds happen **on the rig from its own checkout** — no cross-copying binaries
from another machine. The binary is `build/qemu-system-i386` (meson/ninja via
`./build.sh`); the `vr_*` sources compile with the Vulkan backend (no separate
VR build flag), gated at **runtime** by `vr.enable`. `./build.sh` builds the
right target by default — **the stale-binary trap here is skipping the
rebuild**: before treating any rig result as meaningful, rebuild and confirm
the binary's mtime is newer than your change (a test on yesterday's binary
"disproves" code it never contained). The VULKAN
renderer's display path **requires** NVIDIA GL external-memory interop, so it
will not run under Xvfb/llvmpipe — headless smoke tests use `renderer =
NULL`/OPENGL; real VR validation needs the rig's live X + NVIDIA GL (AGENTS §5).
With `vr.enable = false` (the runtime off-state) behavior must be identical to
upstream xemu.
