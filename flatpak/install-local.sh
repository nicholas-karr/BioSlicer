#!/usr/bin/env bash
# Install a locally-built BioSlicer flatpak bundle.
# Run deploy.sh first to build the bundle.
#
# Usage:
#   ./install-local.sh           # installs bundle for the current arch
#   ./install-local.sh x86_64
#   ./install-local.sh aarch64
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
APP_ID="com.bioslicer.BioSlicer"

ARCH="${1:-$(uname -m)}"
[ "${ARCH}" = "arm64" ] && ARCH=aarch64

BUNDLE="${ROOT_DIR}/BioSlicer-${ARCH}.flatpak"

if [ ! -f "${BUNDLE}" ]; then
    echo "ERROR: Bundle not found: ${BUNDLE}" >&2
    echo "       Run ./flatpak/deploy.sh ${ARCH} first." >&2
    exit 1
fi

# Kill any running instances before reinstalling.
if pgrep -f "flatpak run.*${APP_ID}" &>/dev/null; then
    echo "==> Stopping running BioSlicer instances..."
    pkill -f "flatpak run.*${APP_ID}" 2>/dev/null || true
    sleep 0.5
fi

# Remove any previous installs.
sudo flatpak uninstall --system --noninteractive "${APP_ID}" 2>&1 | grep -v '^$' || true
flatpak uninstall --user --noninteractive "${APP_ID}" 2>&1 | grep -v '^$' || true

# Clean up accumulated synthetic remotes from previous bundle installs.
flatpak remote-list --user 2>/dev/null \
    | awk 'NR>1 && $1 ~ /bioslicer.*-origin/ {print $1}' \
    | xargs -r flatpak remote-delete --user --force 2>/dev/null || true

echo "==> Installing BioSlicer (${ARCH}) from ${BUNDLE}..."
flatpak install --user --noninteractive --reinstall "${BUNDLE}"

echo "==> Done. Run with: flatpak run ${APP_ID}"
