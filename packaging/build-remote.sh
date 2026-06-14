#!/usr/bin/env bash
# build-remote.sh — build the unbox pacman package on a fast remote builder and
# install it here on the slow target.
#
#   1. rsync the working tree to $REMOTE_HOST:~/$REMOTE_DIR
#   2. makepkg -s there (installs build deps via sudo ON the builder — you'll be
#      prompted for that machine's sudo password)
#   3. scp the built .pkg.tar.zst back to $PKGOUT here
#   4. pacman -U here (you'll be prompted for THIS machine's sudo password)
#
# Builder details come from the gitignored packaging/remote.local (template:
# remote.local.example). Re-runnable; safe to Ctrl-C.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LOCAL_REPO="$(cd "$HERE/.." && pwd)"   # repo root (this script lives in packaging/)
# shellcheck disable=SC1091
[ -f "$HERE/remote.local" ] && . "$HERE/remote.local"
: "${REMOTE_HOST:?set REMOTE_HOST (see packaging/remote.local.example)}"
REMOTE_DIR="${REMOTE_DIR:-unbox}"
PKGOUT="${PKGOUT:-/tmp/unbox-pkg}"

echo "==> [1/4] Sync source -> $REMOTE_HOST:$REMOTE_DIR"
rsync -az --info=progress2 \
    --exclude 'build*/' \
    --exclude '.git/' \
    --exclude '.cache/' \
    --exclude 'packaging/remote.local' \
    --exclude 'packaging/src/' \
    --exclude 'packaging/pkg/' \
    --exclude 'packaging/*.pkg.tar.zst' \
    "$LOCAL_REPO/" "$REMOTE_HOST:$REMOTE_DIR/"

echo "==> [2/4] Build package on $REMOTE_HOST (installs makedeps via its sudo)"
# -t: give makepkg a tty so the remote sudo password prompt works.
# -Cf: -C cleans $srcdir first (no stale build dir -> subproject options like
#      toml++ default_library=static always apply), -f overwrites the package.
# Then verify the packaged binary links NO shared toml (extract to a real file;
# readelf cannot read a non-seekable pipe). Abort loudly if it still does.
ssh -t "$REMOTE_HOST" "
    set -e
    cd '$REMOTE_DIR/packaging'
    makepkg -sCf --noconfirm
    f=\$(mktemp)
    bsdtar -xOf unbox-*.pkg.tar.zst usr/bin/unbox > \"\$f\"
    if readelf -d \"\$f\" | grep -qi tomlplusplus; then
        echo '!! packaged binary STILL links libtomlplusplus.so — build did not go static' >&2
        rm -f \"\$f\"; exit 1
    fi
    rm -f \"\$f\"
    echo '==> remote package verified: toml is static (no .so dependency)'
"

echo "==> [3/4] Ferry the built package back -> $PKGOUT"
mkdir -p "$PKGOUT"
scp "$REMOTE_HOST:$REMOTE_DIR/packaging/unbox-*.pkg.tar.zst" "$PKGOUT/"

PKG="$(ls -t "$PKGOUT"/unbox-*.pkg.tar.zst | head -1)"
if [ -z "$PKG" ]; then
    echo "!! no package was produced" >&2
    exit 1
fi

echo "==> [4/4] Install here: $PKG"
sudo pacman -U "$PKG"

# Post-install sanity check on the actually-installed binary.
if ldd /usr/bin/unbox 2>/dev/null | grep -qi 'not found'; then
    echo "!! installed /usr/bin/unbox still has unresolved libraries:" >&2
    ldd /usr/bin/unbox | grep -i 'not found' >&2
    exit 1
fi

echo
echo "==> Done. Installed $(basename "$PKG") — all libraries resolve."
echo "    Binary:  /usr/bin/unbox     Session wrapper:  /usr/bin/start-unbox"
echo "    Assets:  /usr/share/unbox/"
