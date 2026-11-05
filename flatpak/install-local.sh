#!/usr/bin/env bash
# Install a locally-built BioSlicer flatpak from the local OSTree repo.
# Run deploy.sh first to build it.
#
# Usage:
#   ./install-local.sh           # installs the build for the current arch
#   ./install-local.sh x86_64
#   ./install-local.sh aarch64
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
APP_ID="com.bioslicer.BioSlicer"

ARCH="${1:-$(uname -m)}"
[ "${ARCH}" = "arm64" ] && ARCH=aarch64

REPO="$(cd "${ROOT_DIR}" && pwd)/.flatpak-repo-${ARCH}"
REMOTE="bioslicer-local"

if [ ! -f "${REPO}/summary" ]; then
    echo "ERROR: Local repo not found: ${REPO}" >&2
    echo "       Run ./flatpak/deploy.sh --arch ${ARCH} first." >&2
    exit 1
fi

# Kill any running instances before reinstalling.
if pgrep -f "flatpak run.*${APP_ID}" &>/dev/null; then
    echo "==> Stopping running BioSlicer instances..."
    pkill -f "flatpak run.*${APP_ID}" 2>/dev/null || true
    sleep 0.5
fi

# Remove any previous system-wide install (asks for sudo only when there is one).
if flatpak info --system "${APP_ID}" &>/dev/null; then
    echo "==> Removing previous system-wide install..."
    sudo flatpak uninstall --system --noninteractive "${APP_ID}" 2>&1 | grep -v '^$' || true
fi

flatpak remote-add --user --if-not-exists --no-gpg-verify "${REMOTE}" "file://${REPO}"
flatpak remote-modify --user --no-gpg-verify --url="file://${REPO}" "${REMOTE}"

echo "==> Installing BioSlicer (${ARCH}) from ${REPO}..."
if [ "$(flatpak info --user --show-origin "${APP_ID}" 2>/dev/null)" = "${REMOTE}" ]; then
    flatpak update --user --noninteractive -y "${APP_ID}"
else
    flatpak uninstall --user --noninteractive "${APP_ID}" 2>&1 | grep -v '^$' || true
    flatpak install --user --noninteractive -y "${REMOTE}" "${APP_ID}"
fi

echo "==> Done. Run with: flatpak run ${APP_ID}"
