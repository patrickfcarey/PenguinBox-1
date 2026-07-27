#!/usr/bin/env bash
# Deploy the current dev-box VR sources to the rig, rebuild, and apply the OFP
# head-look profile to the test toml. Run from the DEV BOX.
#
# Why scp and not git: the rig can't fetch the private repo (ISS-B09), and the
# only source delta from its build baseline is the handful of files below
# (verified: `git diff --name-only <baseline> HEAD` = exactly these). When the
# rig credential is fixed, prefer `git fetch && git reset --hard origin/vr/mvp`.
#
# H-1 adjacent: never disturbs a live session — aborts on H-2 check, and NEVER
# launches anything (you launch to test). Config is applied only when xemu is
# down, so xemu's on-exit auto-save can't clobber it.
set -euo pipefail

ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
[ -f "$ROOT/.env.local" ] && . "$ROOT/.env.local"
RIG="${RIG_SSH:-pacarey@192.168.68.85}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 $RIG"
DEST="xemu_vr"                       # ~/xemu_vr on the rig

FILES=(
  config_spec.yml
  hw/xbox/nv2a/pgraph/vk/vr_camera.c
  hw/xbox/nv2a/pgraph/vk/vr_manager.c
  hw/xbox/nv2a/pgraph/vk/vr_internal.h
)

echo ">> reachability + H-2 live-session check ($RIG)"
if ! $SSH true 2>/dev/null; then
  echo "ABORT: rig unreachable (asleep / off-network?). Wake it and retry." >&2
  exit 1
fi
live="$($SSH 'pgrep -x pcsx2-qt; pgrep -x mupen64plus; pgrep -f qemu-system-i386' 2>/dev/null || true)"
if [ -n "$live" ]; then
  echo "ABORT (H-2): a live emulator session is on the rig (pids: $(echo $live))." >&2
  echo "            Quit it and take off the headset before deploying." >&2
  exit 1
fi

echo ">> scp ${#FILES[@]} sources + the config applier + the demo launcher"
for f in "${FILES[@]}"; do
  scp -q "$ROOT/$f" "$RIG:$DEST/$f"
done
scp -q "$ROOT/tools/vr/apply-ofp-headlook.py" "$RIG:$DEST/tools/vr/apply-ofp-headlook.py"
# The demo launcher lives in the rig HOME dir (run as ~/demo-pbox.sh).
scp -q "$ROOT/tools/vr/demo-pbox.sh" "$RIG:demo-pbox.sh"
$SSH "chmod +x ~/demo-pbox.sh"

echo ">> rebuild on the rig (ninja regenerates xemu-config.h from config_spec.yml)"
before="$($SSH "stat -c %Y ~/$DEST/build/qemu-system-i386 2>/dev/null || echo 0")"
out="$($SSH "cd ~/$DEST && PATH=\$HOME/.local/bin:\$PATH ninja -C build qemu-system-i386 2>&1" || true)"
echo "$out" | tail -8
after="$($SSH "stat -c %Y ~/$DEST/build/qemu-system-i386 2>/dev/null || echo 0")"
if echo "$out" | grep -qiE 'FAILED:|error:|ninja: error'; then
  echo "ABORT: build errors above." >&2; exit 1
fi
if [ "$after" = "0" ] || [ "$after" = "$before" ]; then
  echo "ABORT: binary did not relink (build likely failed)." >&2; exit 1
fi
$SSH "stat -c 'binary: %y' ~/$DEST/build/qemu-system-i386"

echo ">> apply OFP head-look profile to the toml (xemu is down — safe)"
$SSH "python3 ~/$DEST/tools/vr/apply-ofp-headlook.py ~/xemu-assets/xemu-test.toml"

cat <<'DONE'

READY. On the rig, to test:
    XEMU_VR=1 ~/xemu-assets/boot-test.sh
Durability test: get in-mission, aim (ADS) + look around (head-look should
move the view), then QUIT xemu, relaunch the SAME way, ADS again. If head-look
still works after the relaunch, the pointer anchor is boot-stable -> OFP is our
first durable Tier-3 profile. If yaw is mirrored, flip headlook_sensitivity's
sign; if only pitch is inverted, set headlook_invert_pitch = true (edit the
toml while xemu is down, or re-run apply-ofp-headlook.py after tweaking it).
DONE
