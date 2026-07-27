#!/usr/bin/env bash
# rig-sync-check.sh — is the rig's checkout in sync with origin's branch?
#
# Run FROM THE DEV BOX (it has origin access; the rig may not — ISS-B09). This
# is a read-only DRIFT DETECTOR: it never resets, pushes, or fetches ON THE
# RIG. It refreshes the dev box's origin ref, reads the rig's HEAD read-only
# over SSH, and reports IN-SYNC / BEHIND / AHEAD / DIVERGED so a stale or
# orphaned rig tree can never silently back a build/test (the branch-shaped
# cousin of the "stale binary" false negative — AGENTS.md §5).
#
# ⚠  xemu_vr only unless a repo is explicitly un-frozen. NEVER point this at a
#    frozen repo mid-rewrite (a dev-box fetch is fine, but don't reconcile a
#    frozen clone). Rig connection details come from the git-ignored .env.local
#    ($RIG_SSH) — never hardcoded here (H-8).
#
# Usage:  tools/vr/rig-sync-check.sh [branch] [rig-repo-path]
#   env:  RIG_SSH=user@host  (or set in .env.local)   SKIP_FETCH=1
set -uo pipefail

BRANCH="${1:-vr/mvp}"
RIG_REPO="${2:-\$HOME/xemu_vr}"        # expanded on the rig
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Rig target from .env.local (never committed).
[ -f "$REPO_ROOT/.env.local" ] && . "$REPO_ROOT/.env.local"
if [ -z "${RIG_SSH:-}" ]; then
    echo "ERROR: RIG_SSH not set (put it in $REPO_ROOT/.env.local or the env)." >&2
    exit 2
fi

# 1. Refresh the dev-box origin ref (dev box has creds). xemu_vr is un-frozen.
if [ "${SKIP_FETCH:-0}" != "1" ]; then
    git -C "$REPO_ROOT" fetch -q origin "$BRANCH" 2>/dev/null \
        || echo "WARN: dev-box fetch of origin/$BRANCH failed (using cached ref)." >&2
fi
origin_sha="$(git -C "$REPO_ROOT" rev-parse "origin/$BRANCH" 2>/dev/null)" || {
    echo "ERROR: no local ref origin/$BRANCH on the dev box." >&2; exit 2; }

# 2. Read the rig HEAD (READ-ONLY; no fetch/reset on the rig).
rig_line="$(timeout 15 ssh -o BatchMode=yes -o ConnectTimeout=8 "$RIG_SSH" \
    "git -C $RIG_REPO rev-parse --abbrev-ref HEAD 2>/dev/null; git -C $RIG_REPO rev-parse HEAD 2>/dev/null" 2>/dev/null)"
rig_branch="$(echo "$rig_line" | sed -n 1p)"
rig_sha="$(echo "$rig_line" | sed -n 2p)"
if [ -z "$rig_sha" ]; then
    echo "UNREACHABLE: could not read rig $RIG_REPO HEAD over SSH ($RIG_SSH)." >&2
    exit 2
fi

echo "branch (want):   $BRANCH"
echo "rig branch:      ${rig_branch:-?}"
echo "origin/$BRANCH:  ${origin_sha:0:12}"
echo "rig HEAD:        ${rig_sha:0:12}"

# 3. Classify.
verdict() { echo; echo "VERDICT: $1"; }
if [ "$rig_branch" != "$BRANCH" ]; then
    verdict "WRONG BRANCH — rig is on '$rig_branch', want '$BRANCH'. Reconcile."
elif [ "$rig_sha" = "$origin_sha" ]; then
    verdict "IN SYNC ✓"
    exit 0
elif ! git -C "$REPO_ROOT" cat-file -e "${rig_sha}^{commit}" 2>/dev/null; then
    verdict "DIVERGED/ORPHANED — rig commit not in dev-box history (likely a purged pre-rewrite commit). Reconcile."
elif git -C "$REPO_ROOT" merge-base --is-ancestor "$rig_sha" "$origin_sha" 2>/dev/null; then
    verdict "RIG BEHIND — origin has commits the rig lacks. Reconcile."
elif git -C "$REPO_ROOT" merge-base --is-ancestor "$origin_sha" "$rig_sha" 2>/dev/null; then
    verdict "RIG AHEAD — rig has local commits not on origin. branch-save them, then reconcile/push."
else
    verdict "DIVERGED — rig and origin share no fast-forward path. Reconcile."
fi

cat <<EOF

Reconcile (needs a working rig GitHub credential — ISS-B09; if the rig can't
fetch the private repo, add a PAT/credential-helper or an SSH deploy key first):
  ssh "\$RIG_SSH" 'cd $RIG_REPO && git fetch origin && \\
      git log --oneline origin/$BRANCH..HEAD   # any local commits? branch-save them first: \\
      git branch save/rig-\$(date +%s); \\
      git checkout $BRANCH && git reset --hard origin/$BRANCH'
EOF
exit 1
