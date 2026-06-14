#!/usr/bin/env bash
# setup-distcc.sh — make C/C++ compilation transparently offload to a fast remote
# builder, so a plain `ninja`/`meson test` (and any tool/agent that builds) farms
# compile jobs out automatically, falling back to local when the remote is down.
#
# It works by:
#   * LOCAL : ccache prefix_command=distcc (~/.config/ccache/ccache.conf) +
#             distcc host list (~/.distcc/hosts). These are read by ccache/distcc
#             regardless of shell, so the offload needs NO env vars and is fully
#             transparent (no shell-init, no per-build flags).
#   * REMOTE: a locked-down distccd (systemd) bound to the private interface,
#             accepting ONLY this machine.
# ccache hits stay local/instant; only real compiles go remote; preprocessing and
# linking always run locally.
#
# ---- Setup on a NEW network / set of systems --------------------------------
#   1. On BOTH machines:  install distcc (+ ccache) and ensure the SAME gcc
#      version (distcc plain mode requires matching compilers).
#   2. Ensure passwordless SSH from here to the builder, and sudo on the builder.
#   3.  cp packaging/remote.local.example packaging/remote.local   # fill in IPs
#   4.  packaging/setup-distcc.sh
# Re-runnable and idempotent. Output is tee'd to $LOG.
# -----------------------------------------------------------------------------

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# Machine/network specifics live in the gitignored packaging/remote.local
# (template: remote.local.example). Env vars override the file.
# shellcheck disable=SC1091
[ -f "$HERE/remote.local" ] && . "$HERE/remote.local"
: "${REMOTE_HOST:?set REMOTE_HOST (see packaging/remote.local.example)}"
: "${REMOTE_TS:?set REMOTE_TS — remote builder private IP}"
: "${LOCAL_TS:?set LOCAL_TS — this machine's private IP}"
: "${REMOTE_JOBS:=16}"
PORT="${PORT:-3632}"

LOG=/tmp/distcc-setup.log
: > "$LOG"
exec > >(tee -a "$LOG") 2>&1

echo "######## distcc setup $(date -Is) ########"
echo "remote_host=$REMOTE_HOST  remote_ip=$REMOTE_TS  local_ip=$LOCAL_TS  jobs=$REMOTE_JOBS  port=$PORT"
echo

# ---- 1. local config: transparent for every build tool ----------------------
echo "==> [1/4] local ccache + distcc config"
mkdir -p "$HOME/.config/ccache" "$HOME/.distcc"
cat > "$HOME/.config/ccache/ccache.conf" <<EOF
# On a cache MISS, ccache compiles through distcc (hosts in ~/.distcc/hosts).
# Read for every ccache invocation regardless of shell, so distributed builds
# are transparent. Falls back to local if the remote is unreachable.
prefix_command = distcc
EOF
cat > "$HOME/.distcc/hosts" <<EOF
# Remote builder (private network), then local fallback/overflow. Read when the
# DISTCC_HOSTS env var is unset (so non-login/automated builds use it too).
${REMOTE_TS}/${REMOTE_JOBS},lzo
localhost/4
EOF
echo "  wrote ~/.config/ccache/ccache.conf and ~/.distcc/hosts"
echo

# ---- 2. push the locked-down daemon config to the builder --------------------
echo "==> [2/4] copy distccd config to $REMOTE_HOST:/tmp"
tmpconf="$(mktemp)"; tmpoverride="$(mktemp)"
cat > "$tmpconf" <<EOF
# Managed by packaging/setup-distcc.sh
DISTCC_ARGS="--allow ${LOCAL_TS}/32 --listen ${REMOTE_TS} --port ${PORT} --jobs ${REMOTE_JOBS} --nice 5 --log-level warning"
EOF
cat > "$tmpoverride" <<'EOF'
[Unit]
After=tailscaled.service
[Service]
Restart=on-failure
RestartSec=2
EOF
scp -q "$tmpconf" "$REMOTE_HOST:/tmp/distccd.conf" && echo "  ok: distccd.conf" || { echo "  SCP FAILED"; exit 1; }
scp -q "$tmpoverride" "$REMOTE_HOST:/tmp/distccd-override.conf" && echo "  ok: override.conf" || { echo "  SCP FAILED"; exit 1; }
rm -f "$tmpconf" "$tmpoverride"
echo

# ---- 3. install + enable distccd on the builder (sudo there) -----------------
echo "==> [3/4] install + enable distccd on $REMOTE_HOST (sudo prompt on the builder)"
ssh -t "$REMOTE_HOST" '
  set -e
  sudo install -m644 /tmp/distccd.conf /etc/conf.d/distccd
  sudo mkdir -p /etc/systemd/system/distccd.service.d
  sudo install -m644 /tmp/distccd-override.conf /etc/systemd/system/distccd.service.d/override.conf
  rm -f /tmp/distccd.conf /tmp/distccd-override.conf
  sudo systemctl daemon-reload
  sudo systemctl enable --now distccd
  sudo systemctl restart distccd
  sleep 1
  echo "----- systemctl status -----"; systemctl --no-pager --full status distccd | head -6
  echo "----- listening socket -----"; sudo ss -tlnp | grep ":'"$PORT"'" || echo "NOT LISTENING (FAIL)"
'
echo

# ---- 4. verify offload from this box ----------------------------------------
echo "==> [4/4] verify offload"
echo "--- ccache prefix_command ---"; ccache -p 2>/dev/null | grep -i prefix_command
echo "--- distcc hosts (from file, env unset) ---"; env -u DISTCC_HOSTS distcc --show-hosts 2>&1
td="$(mktemp -d)"
printf '#include <vector>\nint main(){std::vector<int> v(8); return (int)v.size()-8;}\n' > "$td/probe.cpp"
DISTCC_VERBOSE=1 distcc g++ -O2 -std=c++23 -c "$td/probe.cpp" -o "$td/probe.o" 2>"$td/v.txt"; rc=$?
grep -iE "compiled on|exec on|locally|failed to distribute" "$td/v.txt" | head
if [ $rc -eq 0 ] && [ -f "$td/probe.o" ] && ! grep -qiE "running locally|failed to distribute" "$td/v.txt"; then
  echo "RESULT: OK — probe compiled on the remote builder."
else
  echo "RESULT: probe did NOT offload (built locally) — check the daemon/network."
fi
rm -rf "$td"
echo "######## done — full log: $LOG ########"
