#!/usr/bin/env bash
# Build BioSlicer flatpak(s) and deploy bundles to WEB_ROOT.
# Does NOT install locally — use install-local.sh for that.
#
# Usage:
#   ./deploy.sh [options]
#
# Options:
#   --arch ARCH       x86_64 | aarch64 | all  (default: host + aarch64 if cross available)
#   --web-root DIR    Directory to write bundles and install.sh into (default: <repo>/dist)
#   --url URL         Public base URL embedded in install.sh (required)
#   --no-build        Skip the flatpak build; redeploy install.sh only
#   --suffix SUFFIX   Append SUFFIX before .flatpak/.sh in all output filenames (e.g. -dev)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ID="com.bioslicer.BioSlicer"
ROOT_DIR="${SCRIPT_DIR}/.."
WEB_ROOT="${ROOT_DIR}/dist"
PUBLIC_URL=""
SUFFIX=""
TOOLCHAIN="${ROOT_DIR}/cmake/toolchains/aarch64-linux-gnu-clang.cmake"

HOST_ARCH="$(uname -m)"
[ "${HOST_ARCH}" = "arm64" ] && HOST_ARCH=aarch64

# ── Argument parsing ──────────────────────────────────────────────────────────

_arch_arg=""
NO_BUILD=false
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)       _arch_arg="$2"; shift 2 ;;
        --web-root)   WEB_ROOT="$2";  shift 2 ;;
        --url)        PUBLIC_URL="$2"; shift 2 ;;
        --no-build)   NO_BUILD=true;  shift ;;
        --suffix)     SUFFIX="$2";    shift 2 ;;
        *) echo "ERROR: Unknown option '$1'. See usage in script header." >&2; exit 1 ;;
    esac
done

[ -z "${PUBLIC_URL}" ] && { echo "ERROR: --url is required." >&2; exit 1; }

if $NO_BUILD; then
    ARCHS=()
fi

# ── Arch selection ────────────────────────────────────────────────────────────

_can_cross_aarch64=false
if [ "${HOST_ARCH}" = "x86_64" ] && \
   clang -target aarch64-linux-gnu -x c -E /dev/null -o /dev/null &>/dev/null; then
    _can_cross_aarch64=true
fi

if [ -n "${_arch_arg:-}" ]; then
    _req="${_arch_arg}"
    [ "${_req}" = "arm64" ] && _req=aarch64
    case "${_req}" in
        x86_64|aarch64) ARCHS=("${_req}") ;;
        all)
            ARCHS=("${HOST_ARCH}")
            ${_can_cross_aarch64} && [ "${HOST_ARCH}" = "x86_64" ] && ARCHS+=("aarch64")
            ;;
        *)
            echo "ERROR: Unknown arch '${_req}'. Use x86_64, aarch64, or all." >&2
            exit 1 ;;
    esac
else
    # Default: host arch + aarch64 via cross-compile if available.
    ARCHS=("${HOST_ARCH}")
    if ${_can_cross_aarch64} && [ "${HOST_ARCH}" = "x86_64" ]; then
        echo "==> Clang cross-compilation for aarch64 available."
        ARCHS+=("aarch64")
    fi
fi

echo "==> Target arch(s): ${ARCHS[*]}"

# ── Native flatpak-builder build (host arch) ──────────────────────────────────

build_host_flatpak() {
    local ARCH="$1"
    local BUILD_DIR="${ROOT_DIR}/.flatpak-build-${ARCH}"
    local REPO_DIR="${ROOT_DIR}/.flatpak-repo-${ARCH}"
    local STATE_DIR="${ROOT_DIR}/.flatpak-builder-state-${ARCH}"
    local BUNDLE="${ROOT_DIR}/BioSlicer-${ARCH}${SUFFIX}.flatpak"

    echo ""
    echo "==> Building BioSlicer flatpak for ${ARCH} (native)..."

    rm -rf "${REPO_DIR}"
    if [ -d "${STATE_DIR}/rofiles" ]; then
        for mnt in "${STATE_DIR}/rofiles"/rofiles-*; do
            [ -d "${mnt}" ] && fusermount -u "${mnt}" 2>/dev/null || true
        done
        rm -rf "${STATE_DIR}/rofiles"
    fi

    flatpak-builder \
        --user \
        --force-clean \
        --ccache \
        --arch="${ARCH}" \
        --state-dir="${STATE_DIR}" \
        "${BUILD_DIR}" \
        "${SCRIPT_DIR}/${APP_ID}.yml"

    echo ""
    echo "==> BioSlicer (${ARCH}) built."
    echo ""

    echo "==> Exporting to repo and creating bundle (${ARCH})..."
    flatpak build-export --arch="${ARCH}" "${REPO_DIR}" "${BUILD_DIR}"
    flatpak build-bundle --arch="${ARCH}" "${REPO_DIR}" "${BUNDLE}" "${APP_ID}"

    echo "==> Generating SHA256 checksum (${ARCH})..."
    sha256sum "${BUNDLE}" | awk '{print $1}' > "${BUNDLE}.sha256"

    _deploy_bundle "${ARCH}" "${BUNDLE}"
}

# ── Clang cross-compile build (aarch64 from x86_64 host) ─────────────────────

build_cross_aarch64() {
    local ARCH=aarch64
    local STAGE_DIR="${ROOT_DIR}/.flatpak-build-${ARCH}"
    local REPO_DIR="${ROOT_DIR}/.flatpak-repo-${ARCH}"
    local BUNDLE="${ROOT_DIR}/BioSlicer-${ARCH}${SUFFIX}.flatpak"
    local DEPS_BUILD="${ROOT_DIR}/.cross-deps-${ARCH}"
    local DEPS_DEST="${ROOT_DIR}/.cross-deps-${ARCH}-install"
    local APP_BUILD="${ROOT_DIR}/.cross-bioslicer-${ARCH}"
    local DOWNLOAD_CACHE="${ROOT_DIR}/.cross-downloads"
    local WRAPPER_DIR="${ROOT_DIR}/.cross-toolchain-${ARCH}"
    local APPDIR="${STAGE_DIR}/files"

    echo ""
    echo "==> Building BioSlicer for aarch64 via Clang cross-compilation..."

    # ── Sysroot ───────────────────────────────────────────────────────────────
    local SDK_LOC
    SDK_LOC="$(flatpak info --show-location --arch=aarch64 org.gnome.Sdk//49 2>/dev/null || true)"
    if [ -z "${SDK_LOC}" ]; then
        echo "ERROR: aarch64 GNOME SDK not installed." >&2
        echo "       Run: flatpak install --user --arch=aarch64 flathub org.gnome.Sdk//49" >&2
        return 1
    fi
    local SYSROOT="${SDK_LOC}/files"
    echo "    Sysroot : ${SYSROOT}"

    # ── Linker ────────────────────────────────────────────────────────────────
    local LLD_PATH=""
    for _v in 20 15 14 12; do
        if [ -x "/usr/bin/ld.lld-${_v}" ]; then
            LLD_PATH="/usr/bin/ld.lld-${_v}"
            break
        fi
    done
    command -v ld.lld &>/dev/null && [ -z "${LLD_PATH}" ] && LLD_PATH="$(command -v ld.lld)"
    [ -n "${LLD_PATH}" ] && echo "    Linker  : ${LLD_PATH}" || echo "    Linker  : system default"

    # ── Staging sysroot ───────────────────────────────────────────────────────
    local STAGING="${WRAPPER_DIR}/sysroot"
    mkdir -p "${STAGING}"
    ln -sfn "${SYSROOT}"         "${STAGING}/usr"
    ln -sfn "${SYSROOT}/lib"     "${STAGING}/lib"
    ln -sfn "${SYSROOT}/include" "${STAGING}/include"
    ln -sfn . "${SYSROOT}/usr" 2>/dev/null || true

    if [ ! -f "${SYSROOT}/include/GL/glu.h" ] && [ -f "/usr/include/GL/glu.h" ]; then
        cp /usr/include/GL/glu.h "${SYSROOT}/include/GL/glu.h"
    fi

    # ── Compiler wrapper scripts ──────────────────────────────────────────────
    mkdir -p "${WRAPPER_DIR}"
    local CC_WRAPPER="${WRAPPER_DIR}/cc"
    local CXX_WRAPPER="${WRAPPER_DIR}/cxx"

    local LLD_LINE=""
    if [ -n "${LLD_PATH}" ]; then
        LLD_LINE="    -fuse-ld=${LLD_PATH} \\"
    fi

    local _WRAPPER_PREAMBLE
    _WRAPPER_PREAMBLE=$(cat <<'PREAMBLE'
_linking=true
for _a; do
    case "$_a" in -c|-E|-S|-M|-MM) _linking=false; break;; esac
done
PREAMBLE
)

    local _CC_BIN=clang _CXX_BIN=clang++
    for _v in 20 19 18 17; do
        if [ -x "/usr/bin/clang-${_v}" ]; then
            _CC_BIN="clang-${_v}"
            _CXX_BIN="clang++-${_v}"
            break
        fi
    done

    cat > "${CC_WRAPPER}" <<EOF
#!/bin/bash
${_WRAPPER_PREAMBLE}
if \$_linking; then
exec ${_CC_BIN} --target=aarch64-linux-gnu \\
    --sysroot="${STAGING}" \\
    --gcc-toolchain="${SYSROOT}" \\
${LLD_LINE}
    "\$@" \\
    -isystem "${SYSROOT}/lib/aarch64-linux-gnu/include"
else
exec ${_CC_BIN} --target=aarch64-linux-gnu \\
    --sysroot="${STAGING}" \\
    --gcc-toolchain="${SYSROOT}" \\
    "\$@" \\
    -isystem "${SYSROOT}/lib/aarch64-linux-gnu/include"
fi
EOF
    cat > "${CXX_WRAPPER}" <<EOF
#!/bin/bash
${_WRAPPER_PREAMBLE}
if \$_linking; then
exec ${_CXX_BIN} --target=aarch64-linux-gnu \\
    --sysroot="${STAGING}" \\
    --gcc-toolchain="${SYSROOT}" \\
${LLD_LINE}
    "\$@" \\
    -isystem "${SYSROOT}/lib/aarch64-linux-gnu/include"
else
exec ${_CXX_BIN} --target=aarch64-linux-gnu \\
    --sysroot="${STAGING}" \\
    --gcc-toolchain="${SYSROOT}" \\
    "\$@" \\
    -isystem "${SYSROOT}/lib/aarch64-linux-gnu/include"
fi
EOF
    chmod +x "${CC_WRAPPER}" "${CXX_WRAPPER}"
    echo "    CC      : ${CC_WRAPPER}"

    export BIOSLICER_CROSS_CC="${CC_WRAPPER}"
    export BIOSLICER_CROSS_CXX="${CXX_WRAPPER}"
    export BIOSLICER_CROSS_LLD="${LLD_PATH}"
    export BIOSLICER_AARCH64_SYSROOT="${STAGING}"

    export PKG_CONFIG_SYSROOT_DIR="${STAGING}"
    export PKG_CONFIG_LIBDIR="${SYSROOT}/lib/aarch64-linux-gnu/pkgconfig:${SYSROOT}/share/pkgconfig"
    export PKG_CONFIG_PATH=""

    # ── ccache ────────────────────────────────────────────────────────────────
    local CCACHE_LAUNCHER=""
    if command -v ccache &>/dev/null; then
        export CCACHE_DIR="${ROOT_DIR}/.cross-ccache-${ARCH}"
        CCACHE_LAUNCHER="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
        echo "    ccache  : ${CCACHE_DIR}"
    fi

    # ── Build deps ─────────────────────────────────────────────────────────────
    echo "==> Cross-compiling deps for aarch64..."
    mkdir -p "${DEPS_BUILD}" "${DEPS_DEST}" "${DOWNLOAD_CACHE}"

    cmake "${ROOT_DIR}/deps" \
        -B "${DEPS_BUILD}" \
        -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DDEP_WX_GTK3=1 \
        -DDEP_DOWNLOAD_DIR="${DOWNLOAD_CACHE}" \
        -DDESTDIR="${DEPS_DEST}" \
        ${CCACHE_LAUNCHER}

    cmake --build "${DEPS_BUILD}"

    # ── Initialise flatpak stage dir ──────────────────────────────────────────
    rm -rf "${STAGE_DIR}"
    flatpak build-init --arch=aarch64 "${STAGE_DIR}" "${APP_ID}" \
        org.gnome.Sdk//49 org.gnome.Platform//49
    mkdir -p "${APPDIR}"

    # ── Build BioSlicer ────────────────────────────────────────────────────────
    echo "==> Cross-compiling BioSlicer for aarch64..."

    sed -i 's/+UNKNOWN/+bioslicer/' "${ROOT_DIR}/version.inc" 2>/dev/null || true

    mkdir -p "${APP_BUILD}"
    cmake "${ROOT_DIR}" \
        -B "${APP_BUILD}" \
        -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DCMAKE_INSTALL_PREFIX=/app \
        -DCMAKE_PREFIX_PATH="${DEPS_DEST}/usr/local" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSLIC3R_PCH=OFF \
        -DSLIC3R_FHS=ON \
        -DSLIC3R_ASAN=OFF \
        -DSLIC3R_GTK=3 \
        -DSLIC3R_STATIC=ON \
        -DSLIC3R_BUILD_TESTS=OFF \
        -DSLIC3R_ENC_CHECK=OFF \
        -DSLIC3R_DESKTOP_INTEGRATION=OFF \
        -DwxWidgets_CONFIG_EXECUTABLE="${DEPS_DEST}/usr/local/bin/wx-config" \
        ${CCACHE_LAUNCHER}

    cmake --build "${APP_BUILD}"
    cmake --install "${APP_BUILD}" --prefix "${APPDIR}"

    # ── Post-install: localization ─────────────────────────────────────────────
    if [ -d "${APPDIR}/share/PrusaSlicer/localization" ]; then
        mkdir -p "${APPDIR}/share/runtime/locale"
        for i in $(ls "${APPDIR}/share/PrusaSlicer/localization"); do
            lang="${i%[_@]*}"
            mkdir -p "${APPDIR}/share/runtime/locale/${lang}"
            mv "${APPDIR}/share/PrusaSlicer/localization/${i}" \
               "${APPDIR}/share/runtime/locale/${lang}/"
            ln -rs "${APPDIR}/share/runtime/locale/${lang}/${i}" \
                   "${APPDIR}/share/PrusaSlicer/localization/${i}"
        done
    fi

    # ── Post-install: metadata, icons, desktop file ───────────────────────────
    install -Dm644 "${SCRIPT_DIR}/com.bioslicer.BioSlicer.metainfo.xml" \
        "${APPDIR}/share/metainfo/com.bioslicer.BioSlicer.metainfo.xml"
    install -Dm644 "${ROOT_DIR}/resources/icons/PrusaSlicer.svg" \
        "${APPDIR}/share/icons/hicolor/scalable/apps/com.bioslicer.BioSlicer.svg"
    install -Dm644 "${ROOT_DIR}/resources/icons/PrusaSlicer.png" \
        "${APPDIR}/share/icons/hicolor/256x256/apps/com.bioslicer.BioSlicer.png"

    local DESKTOP="${APP_BUILD}/com.bioslicer.BioSlicer.desktop"
    cp "${ROOT_DIR}/src/platform/unix/PrusaSlicer.desktop" "${DESKTOP}"
    sed -i 's/Name=PrusaSlicer/Name=BioSlicer/g'                                  "${DESKTOP}"
    sed -i 's/^\(MimeType=.*\)/\1x-scheme-handler\/bioslicer;/g'                  "${DESKTOP}"
    sed -i 's/Exec=prusa-slicer %F/Exec=entrypoint --single-instance-on-url %u/g' "${DESKTOP}"
    sed -i 's/Exec=prusa-slicer/Exec=entrypoint/g'                                "${DESKTOP}"
    sed -i 's/Icon=PrusaSlicer/Icon=com.bioslicer.BioSlicer/g'                    "${DESKTOP}"
    install -Dm644 "${DESKTOP}" \
        "${APPDIR}/share/applications/com.bioslicer.BioSlicer.desktop"

    install "${SCRIPT_DIR}/set-dark-theme-variant.py" "${APPDIR}/bin/"
    install "${SCRIPT_DIR}/uses-dark-theme.py"        "${APPDIR}/bin/"
    install "${SCRIPT_DIR}/entrypoint"                "${APPDIR}/bin/"
    install "${SCRIPT_DIR}/umount"                    "${APPDIR}/bin/"

    _pyver=$(python3 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
    pip3 install --quiet --no-deps --no-build-isolation \
        --ignore-installed \
        --target="${APPDIR}/lib/python${_pyver}/site-packages" \
        "six==1.17.0" "python-xlib==0.33"
    unset _pyver

    mkdir -p "${APPDIR}/share/PrusaSlicer/scripts"
    install -Dm644 "${ROOT_DIR}/scripts/hybrid_3mf_utils.py" \
        "${APPDIR}/share/PrusaSlicer/scripts/hybrid_3mf_utils.py"
    install -Dm644 "${ROOT_DIR}/scripts/gen_sla_demo_models.py" \
        "${APPDIR}/share/PrusaSlicer/scripts/gen_sla_demo_models.py"

    mkdir -p "${APPDIR}/share/mime/packages"
    install -m644 "${SCRIPT_DIR}/com.bioslicer.BioSlicer.mime.xml" \
        "${APPDIR}/share/mime/packages/"
    update-mime-database "${APPDIR}/share/mime" 2>/dev/null || true

    flatpak build-finish \
        --command=entrypoint \
        --share=ipc \
        --socket=x11 \
        --socket=wayland \
        --share=network \
        --device=all \
        --filesystem=home \
        --filesystem=xdg-run/gvfs \
        --filesystem=/run/media \
        --filesystem=/media \
        "--own-name=com.bioslicer.bioslicer.*" \
        --system-talk-name=org.freedesktop.UDisks2 \
        "--talk-name=org.freedesktop.DBus.Introspectable.*" \
        "--talk-name=com.bioslicer.bioslicer.InstanceCheck.*" \
        --env=PRUSA_SLICER_DARK_THEME=false \
        "${STAGE_DIR}"

    rm -rf "${REPO_DIR}"
    flatpak build-export --arch=aarch64 "${REPO_DIR}" "${STAGE_DIR}"
    flatpak build-bundle --arch=aarch64 "${REPO_DIR}" "${BUNDLE}" "${APP_ID}"

    echo "==> Generating SHA256 checksum (aarch64)..."
    sha256sum "${BUNDLE}" | awk '{print $1}' > "${BUNDLE}.sha256"

    _deploy_bundle "aarch64" "${BUNDLE}"
}

# ── Deploy helper ─────────────────────────────────────────────────────────────

_deploy_bundle() {
    local ARCH="$1" BUNDLE="$2"
    echo "==> Deploying BioSlicer-${ARCH}${SUFFIX}.flatpak to ${WEB_ROOT}..."
    mkdir -p "${WEB_ROOT}"
    cp "${BUNDLE}"         "${WEB_ROOT}/BioSlicer-${ARCH}${SUFFIX}.flatpak"
    cp "${BUNDLE}.sha256"  "${WEB_ROOT}/BioSlicer-${ARCH}${SUFFIX}.flatpak.sha256"
    chmod 644 "${WEB_ROOT}/BioSlicer-${ARCH}${SUFFIX}.flatpak" \
              "${WEB_ROOT}/BioSlicer-${ARCH}${SUFFIX}.flatpak.sha256"
}

if ! $NO_BUILD; then

# ── Install runtimes ──────────────────────────────────────────────────────────

echo "==> Installing GNOME Platform 49 runtimes (if not already installed)..."
flatpak install --user --noninteractive flathub \
    org.gnome.Platform//49 org.gnome.Sdk//49 || true

for ARCH in "${ARCHS[@]}"; do
    if [ "${ARCH}" != "${HOST_ARCH}" ]; then
        echo "==> Installing GNOME Platform 49 runtimes for ${ARCH}..."
        flatpak install --user --noninteractive --arch="${ARCH}" flathub \
            org.gnome.Platform//49 org.gnome.Sdk//49 || true
    fi
done

# ── Build each arch ───────────────────────────────────────────────────────────

for ARCH in "${ARCHS[@]}"; do
    if [ "${ARCH}" = "${HOST_ARCH}" ]; then
        build_host_flatpak "${ARCH}"
    elif [ "${ARCH}" = "aarch64" ] && ${_can_cross_aarch64}; then
        build_cross_aarch64
    else
        echo "WARNING: No build path for ${ARCH} on host ${HOST_ARCH} — skipping." >&2
    fi
done

# Keep BioSlicer${SUFFIX}.flatpak pointing to the x86_64 bundle for backward compat.
_built_x86=false
for ARCH in "${ARCHS[@]}"; do [ "${ARCH}" = "x86_64" ] && _built_x86=true; done
if ${_built_x86}; then
    echo "==> Updating generic BioSlicer${SUFFIX}.flatpak (→ x86_64)..."
    cp "${WEB_ROOT}/BioSlicer-x86_64${SUFFIX}.flatpak"        "${WEB_ROOT}/BioSlicer${SUFFIX}.flatpak"
    cp "${WEB_ROOT}/BioSlicer-x86_64${SUFFIX}.flatpak.sha256" "${WEB_ROOT}/BioSlicer${SUFFIX}.flatpak.sha256"
    chmod 644 "${WEB_ROOT}/BioSlicer${SUFFIX}.flatpak" "${WEB_ROOT}/BioSlicer${SUFFIX}.flatpak.sha256"
fi

fi # --no-build

echo "==> Deploying install${SUFFIX}.sh (url: ${PUBLIC_URL})..."
sed -e "s|__BUNDLE_BASE__|${PUBLIC_URL}|g" \
    -e "s|__BUNDLE_SUFFIX__|${SUFFIX}|g" \
    "${SCRIPT_DIR}/install.sh" > "${WEB_ROOT}/install${SUFFIX}.sh"
chmod 644 "${WEB_ROOT}/install${SUFFIX}.sh"

echo ""
echo "==> Done. Published at:"
if ! $NO_BUILD; then
    for ARCH in "${ARCHS[@]}"; do
        echo "    ${PUBLIC_URL}/BioSlicer-${ARCH}${SUFFIX}.flatpak"
    done
    ${_built_x86} && echo "    ${PUBLIC_URL}/BioSlicer${SUFFIX}.flatpak  (alias → x86_64)"
fi
echo "    ${PUBLIC_URL}/install${SUFFIX}.sh"
echo ""
if ! $NO_BUILD; then
    echo "    To install locally: flatpak/install-local.sh [arch]"
fi
