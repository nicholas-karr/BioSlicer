#!/usr/bin/env bash
# Build BioSlicer flatpak(s) and publish to WEB_ROOT: an OSTree repo (fast incremental
# updates, for self-hosted/test builds) and, with --bundle, single-file .flatpak release
# assets (what gets attached to a GitHub Release).
# Does NOT install locally — use install-local.sh for that.
#
# Usage:
#   ./deploy.sh [options]
#
# Options:
#   --arch ARCH       x86_64 | aarch64 | all  (default: host + aarch64 if cross available)
#   --web-root DIR    Directory to write repo-<arch> and install.sh into (default: <repo>/dist)
#   --url URL         Public base URL embedded in the published install.sh copy (optional;
#                     without it no install.sh copy is written)
#   --bundle          Also write BioSlicer-<arch><SUFFIX>.flatpak + .sha256 to WEB_ROOT
#                     (slow: build-bundle compresses on one thread, ~90s). These are the
#                     GitHub Release assets the repo's install.sh downloads by default.
#   --no-build        Skip the flatpak build; redeploy install.sh only
#   --deltas          Also generate static deltas (+~15s here, halves the remote download;
#                     worth it only on slow links)
#   --suffix SUFFIX   Publish install<SUFFIX>.sh and repo-<arch><SUFFIX> (e.g. -dev) as a
#                     separate channel
set -e
set -o pipefail
# Without this, `set -e` is silently OFF inside command-substitution subshells
# (e.g. `x="$(f)"`), so a failing command in the middle of a function called
# that way doesn't abort — execution just continues to the next statement and
# the function's exit status ends up reflecting whatever ran last, not the
# failure. _compute_build_hash() is only ever invoked as `$(_compute_build_hash
# ...)`, so without this its internal failures (a broken symlink hitting
# sha256sum, a failing git call) would be silently masked despite `pipefail`.
shopt -s inherit_errexit

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
DELTAS=false
BUNDLE=false
while [ $# -gt 0 ]; do
    case "$1" in
        --arch)       _arch_arg="$2"; shift 2 ;;
        --web-root)   WEB_ROOT="$2";  shift 2 ;;
        --url)        PUBLIC_URL="$2"; shift 2 ;;
        --no-build)   NO_BUILD=true;  shift ;;
        --deltas)     DELTAS=true;    shift ;;
        --bundle)     BUNDLE=true;    shift ;;
        --suffix)     SUFFIX="$2";    shift 2 ;;
        *) echo "ERROR: Unknown option '$1'. See usage in script header." >&2; exit 1 ;;
    esac
done


if $NO_BUILD; then
    # Nothing below this arch-selection block is used when skipping the build
    # (the build loop is separately guarded by `if ! $NO_BUILD`), so skip it
    # entirely rather than computing it and immediately discarding it — that
    # previously also printed a misleading "Target arch(s)" line and ran an
    # unnecessary clang cross-compile probe on --no-build.
    ARCHS=()
else
    # ── Arch selection ────────────────────────────────────────────────────────

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
fi

# ── Phase timing ──────────────────────────────────────────────────────────────

_phase_t0=${SECONDS}
_phase() {  # _phase LABEL — print elapsed time since the previous _phase call
    local now=${SECONDS}
    echo "==> [timing] $1: $((now - _phase_t0))s"
    _phase_t0=${now}
}

# Run a long, silent command with a heartbeat every 10s so the user can tell it
# is still working. If OUT_FILE is given, its growing size is shown too.
#   _with_heartbeat LABEL OUT_FILE cmd args...
_with_heartbeat() {
    local label="$1" out_file="$2"; shift 2
    local t0=${SECONDS} hb_pid rc=0
    (
        while sleep 10; do
            size=""
            if [ -n "${out_file}" ] && [ -e "${out_file}" ]; then
                size=", $(du -h "${out_file}" | cut -f1) written"
            fi
            echo "    ... ${label} still running ($((SECONDS - t0))s${size})"
        done
    ) &
    hb_pid=$!
    "$@" || rc=$?
    kill "${hb_pid}" 2>/dev/null || true
    wait "${hb_pid}" 2>/dev/null || true
    if [ "${rc}" -ne 0 ]; then
        echo "==> ${label} FAILED (exit ${rc}) after $((SECONDS - t0))s" >&2
        return "${rc}"
    fi
    echo "==> ${label} done in $((SECONDS - t0))s"
}

# Export BUILD_DIR into a persistent OSTree repo (so export only writes changed
# objects) and publish it; clients pull just the objects that changed.
#   _export_repo ARCH REPO_DIR BUILD_DIR
_export_repo() {
    local ARCH="$1" REPO_DIR="$2" BUILD_DIR="$3"
    echo "==> [${ARCH}] Exporting build to OSTree repo (${REPO_DIR})..."
    # archive-z2 repos zlib-compress every file (default level 6) *before* checking
    # whether the object already exists, so an unchanged 300MB tree still costs
    # ~6s of pure compression. Level 1 is ~2x faster for ~8% larger objects.
    if command -v ostree &>/dev/null; then
        [ -d "${REPO_DIR}/objects" ] || ostree init --repo="${REPO_DIR}" --mode=archive-z2
        ostree --repo="${REPO_DIR}" config set archive.zlib-level 1
    fi
    _with_heartbeat "[${ARCH}] build-export" "" \
        flatpak build-export --arch="${ARCH}" --disable-fsync --no-update-summary \
            "${REPO_DIR}" "${BUILD_DIR}"
    _with_heartbeat "[${ARCH}] build-update-repo" "" \
        flatpak build-update-repo --prune --prune-depth=1 \
            $($DELTAS && echo --generate-static-deltas) "${REPO_DIR}"
    _publish_repo "${ARCH}" "${REPO_DIR}"
    if $BUNDLE; then
        local BUNDLE_FILE="${WEB_ROOT}/BioSlicer-${ARCH}${SUFFIX}.flatpak"
        mkdir -p "${WEB_ROOT}"
        echo "==> [${ARCH}] Creating ${BUNDLE_FILE##*/} (compression is single-threaded and quiet; can take minutes)..."
        rm -f "${BUNDLE_FILE}"  # so the heartbeat size reflects this run
        _with_heartbeat "[${ARCH}] build-bundle" "${BUNDLE_FILE}" \
            flatpak build-bundle --arch="${ARCH}" "${REPO_DIR}" "${BUNDLE_FILE}" "${APP_ID}"
        sha256sum "${BUNDLE_FILE}" | awk '{print $1}' > "${BUNDLE_FILE}.sha256"
        chmod 644 "${BUNDLE_FILE}" "${BUNDLE_FILE}.sha256"
        echo "==> [${ARCH}] Bundle size: $(du -h "${BUNDLE_FILE}" | cut -f1)"
    fi
}

# Mirror the repo into the web root (objects are content-addressed, so rsync
# only copies what changed).
_publish_repo() {
    local ARCH="$1" REPO_DIR="$2" dest
    dest="${WEB_ROOT}/repo-${ARCH}${SUFFIX}"
    mkdir -p "${dest}"
    _with_heartbeat "[${ARCH}] publish repo" "" \
        rsync -a --delete --chmod=D755,F644 --exclude=/tmp --exclude=/.lock \
            "${REPO_DIR}/" "${dest}/"
}

# Unmount stale flatpak-builder rofiles FUSE mounts left by an interrupted run.
_clear_rofiles() {
    local STATE_DIR="$1" mnt
    if [ -d "${STATE_DIR}/rofiles" ]; then
        for mnt in "${STATE_DIR}/rofiles"/rofiles-*; do
            [ -d "${mnt}" ] && fusermount -u "${mnt}" 2>/dev/null || true
        done
        rm -rf "${STATE_DIR}/rofiles"
    fi
}

# ── Deps source tarball ───────────────────────────────────────────────────────

# The manifest's BioSlicerDeps module builds from flatpak/bioslicer-deps-src.tar.gz, an
# archive of deps/ and cmake/. flatpak-builder keys its module cache for a local archive on
# the manifest's sha256 (not on the file's content), so the two must always agree.
#
# This regenerates the archive from the files git knows about (tracked, plus untracked ones
# that are not ignored), so local build trees such as deps/build-default never get in, and
# with fixed order, timestamps, owners and modes, so the same sources give the same bytes on
# every machine. It then prints the manifest to build from: the committed one when its
# sha256 matches, otherwise a generated copy carrying the real checksum.
_prepare_deps_source() {
    local tarball="${SCRIPT_DIR}/bioslicer-deps-src.tar.gz"
    local manifest="${SCRIPT_DIR}/${APP_ID}.yml"
    local tmp="${tarball}.tmp.$$"
    local path actual recorded
    local -a files=()

    while IFS= read -r -d '' path; do
        if [ -f "${ROOT_DIR}/${path}" ] || [ -L "${ROOT_DIR}/${path}" ]; then
            files+=("${path}")
        fi
    done < <(git -C "${ROOT_DIR}" ls-files --cached --others --exclude-standard -z -- deps cmake \
                 | LC_ALL=C sort -z)
    if [ "${#files[@]}" -eq 0 ]; then
        echo "ERROR: No deps/ or cmake/ sources found in ${ROOT_DIR}." >&2
        return 1
    fi

    printf '%s\0' "${files[@]}" \
        | tar -C "${ROOT_DIR}" --null --no-recursion --files-from=- \
              --format=gnu --mtime='2026-01-01 00:00:00 UTC' \
              --owner=0 --group=0 --numeric-owner --mode='a+rX,u+w,go-w' -cf - \
        | gzip -n > "${tmp}"
    if [ -f "${tarball}" ] && cmp -s "${tmp}" "${tarball}"; then
        rm -f "${tmp}"
    else
        mv "${tmp}" "${tarball}"
        echo "==> Regenerated ${tarball##*/} from deps/ and cmake/." >&2
    fi

    actual="$(sha256sum "${tarball}" | awk '{print $1}')"
    recorded="$(awk '/path: bioslicer-deps-src\.tar\.gz/ {found=1; next}
                     found && /sha256:/ {print $2; exit}' "${manifest}")"
    if [ "${actual}" = "${recorded}" ]; then
        echo "${manifest}"
        return 0
    fi

    # deps/ or cmake/ changed since the manifest's checksum was recorded. Build from a copy
    # that carries the real one, so the deps module is rebuilt exactly when its sources change.
    local generated="${SCRIPT_DIR}/.generated-${APP_ID}.yml"
    sed "/path: bioslicer-deps-src\.tar\.gz/{n;s/sha256: .*/sha256: ${actual}/}" \
        "${manifest}" > "${generated}"
    {
        echo "==> deps/ or cmake/ differ from the sha256 recorded in ${manifest##*/}."
        echo "    Building from a generated manifest. To record the change, set that sha256 to:"
        echo "    ${actual}"
    } >&2
    echo "${generated}"
}

# ── Native flatpak-builder build (host arch) ──────────────────────────────────

build_host_flatpak() {
    local ARCH="$1"
    local BUILD_DIR="${ROOT_DIR}/.flatpak-build-${ARCH}"
    local REPO_DIR="${ROOT_DIR}/.flatpak-repo-${ARCH}"
    local STATE_DIR="${ROOT_DIR}/.flatpak-builder-state-${ARCH}"
    local BUILD_INPUT_HASH
    BUILD_INPUT_HASH="$(_compute_build_hash "${ARCH}")"

    echo ""
    echo "==> Building BioSlicer flatpak for ${ARCH} (native)..."

    if _is_build_current "${ARCH}" "${BUILD_INPUT_HASH}"; then
        echo "==> [${ARCH}] No changes since last build — skipping rebuild."
        _publish_repo "${ARCH}" "${REPO_DIR}"
        return 0
    fi

    _clear_rofiles "${STATE_DIR}"
    local MANIFEST
    MANIFEST="$(_prepare_deps_source)"
    _phase "hash + up-to-date check"

    flatpak-builder \
        --user \
        --force-clean \
        --ccache \
        --arch="${ARCH}" \
        --state-dir="${STATE_DIR}" \
        "${BUILD_DIR}" \
        "${MANIFEST}"
    _phase "flatpak-builder"

    echo ""
    echo "==> BioSlicer (${ARCH}) built."
    echo ""

    _export_repo "${ARCH}" "${REPO_DIR}" "${BUILD_DIR}"
    _phase "export + publish"

    _save_build_hash "${ARCH}" "${BUILD_INPUT_HASH}"
}

# ── Clang cross-compile build (aarch64 from x86_64 host) ─────────────────────

build_cross_aarch64() {
    local ARCH=aarch64
    local STAGE_DIR="${ROOT_DIR}/.flatpak-build-${ARCH}"
    local REPO_DIR="${ROOT_DIR}/.flatpak-repo-${ARCH}"
    local DEPS_BUILD="${ROOT_DIR}/.cross-deps-${ARCH}"
    local DEPS_DEST="${ROOT_DIR}/.cross-deps-${ARCH}-install"
    local APP_BUILD="${ROOT_DIR}/.cross-bioslicer-${ARCH}"
    local DOWNLOAD_CACHE="${ROOT_DIR}/.cross-downloads"
    local WRAPPER_DIR="${ROOT_DIR}/.cross-toolchain-${ARCH}"
    local APPDIR="${STAGE_DIR}/files"
    local BUILD_INPUT_HASH
    BUILD_INPUT_HASH="$(_compute_build_hash "${ARCH}")"

    echo ""
    echo "==> Building BioSlicer for aarch64 via Clang cross-compilation..."

    if _is_build_current "${ARCH}" "${BUILD_INPUT_HASH}"; then
        echo "==> [aarch64] No changes since last build — skipping rebuild."
        _publish_repo "aarch64" "${REPO_DIR}"
        return 0
    fi

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
    local LLD_PATH
    LLD_PATH="$(_detect_cross_lld)"
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

    local _CC_BIN
    _CC_BIN="$(_detect_cross_cc)"
    local _CXX_BIN="${_CC_BIN/clang/clang++}"

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
        "--own-name=com.prusa3d.prusaslicer.*" \
        --system-talk-name=org.freedesktop.UDisks2 \
        "--talk-name=org.freedesktop.DBus.Introspectable.*" \
        "--talk-name=com.prusa3d.prusaslicer.InstanceCheck.*" \
        --env=PRUSA_SLICER_DARK_THEME=false \
        "${STAGE_DIR}"

    _export_repo aarch64 "${REPO_DIR}" "${STAGE_DIR}"
    _save_build_hash "aarch64" "${BUILD_INPUT_HASH}"
}

# ── Cross-toolchain detection (shared by the build and its fingerprint, so ────
#    they cannot silently drift apart) ──────────────────────────────────────────

_detect_cross_cc() {
    local v
    for v in 20 19 18 17; do
        if [ -x "/usr/bin/clang-${v}" ]; then
            echo "clang-${v}"
            return 0
        fi
    done
    echo "clang"
}

_detect_cross_lld() {
    local v
    for v in 20 15 14 12; do
        if [ -x "/usr/bin/ld.lld-${v}" ]; then
            echo "/usr/bin/ld.lld-${v}"
            return 0
        fi
    done
    command -v ld.lld 2>/dev/null || true
}

# ── Build-hash helpers (fast no-op when nothing changed) ──────────────────────

_compute_build_hash() {
    local arch="${1:-unknown}" path
    {
        printf 'schema=4\narch=%s\nsuffix=%s\nhost=%s\nflatpak=%s\n' \
            "${arch}" "${SUFFIX}" "$(uname -m)" \
            "$(flatpak --version 2>/dev/null || echo unavailable)"
        # The manifest text (tracked, already hashed below) pins a runtime
        # *branch*, e.g. org.gnome.Sdk//49 — but a `flatpak update` can move
        # that branch to a new commit without changing any tracked file, which
        # silently changes what actually gets built. Pin the installed commits.
        printf 'sdk_commit=%s\nplatform_commit=%s\n' \
            "$(flatpak info --arch="${arch}" org.gnome.Sdk//49 2>/dev/null \
                | awk -F': ' '/^Commit/{print $2; exit}')" \
            "$(flatpak info --arch="${arch}" org.gnome.Platform//49 2>/dev/null \
                | awk -F': ' '/^Commit/{print $2; exit}')"
        # The aarch64 path additionally cross-compiles with an autodetected
        # host clang/lld; a system compiler upgrade changes the build without
        # changing any tracked file either.
        if [ "${arch}" = "aarch64" ]; then
            local cc lld
            cc="$(_detect_cross_cc)"
            lld="$(_detect_cross_lld)"
            printf 'cross_cc=%s\ncross_cc_version=%s\n' \
                "${cc}" "$("${cc}" --version 2>/dev/null | head -1 || echo unavailable)"
            printf 'cross_lld=%s\ncross_lld_version=%s\n' \
                "${lld}" \
                "$([ -n "${lld}" ] && "${lld}" --version 2>/dev/null | head -1 || echo unavailable)"
        fi
        # Hash the content and path of every tracked or untracked, non-ignored
        # input. This includes staged changes, deletions, new files, executable
        # bits through Git's index listing, and submodule worktree state.
        # Regular files/symlinks are collected and hashed in one batched
        # sha256sum call (a fork per file was ~6s per invocation on this tree).
        git -C "${ROOT_DIR}" ls-files --stage
        local -a _files=()
        while IFS= read -r -d '' path; do
            if [ -f "${ROOT_DIR}/${path}" ] || [ -L "${ROOT_DIR}/${path}" ]; then
                _files+=("${path}")
                continue
            fi
            printf 'path=%s\n' "${path}"
            if [ -d "${ROOT_DIR}/${path}" ] \
                    && git -C "${ROOT_DIR}/${path}" rev-parse --is-inside-work-tree &>/dev/null; then
                git -C "${ROOT_DIR}/${path}" rev-parse HEAD
                git -C "${ROOT_DIR}/${path}" status --porcelain=v1 --untracked-files=all
            else
                printf 'deleted\n'
            fi
        done < <(git -C "${ROOT_DIR}" ls-files --cached --others --exclude-standard -z | LC_ALL=C sort -z)
        if [ "${#_files[@]}" -gt 0 ]; then
            printf '%s\0' "${_files[@]}" | (cd "${ROOT_DIR}" && xargs -0 sha256sum --)
        fi
    } | sha256sum | awk '{print $1}'
}

_hash_file() { echo "${ROOT_DIR}/.flatpak-build-${1}-hash"; }

_is_build_current() {
    local ARCH="$1" HASH="$2"   # HASH: the caller's already-computed input hash
    local hf; hf="$(_hash_file "${ARCH}")"
    [ -f "${hf}" ] && [ "${HASH}" = "$(cat "${hf}")" ] || return 1
    [ -f "${WEB_ROOT}/repo-${ARCH}${SUFFIX}/summary" ] && \
        [ -f "${ROOT_DIR}/.flatpak-repo-${ARCH}/summary" ] || return 1
    ! $BUNDLE || [ -f "${WEB_ROOT}/BioSlicer-${ARCH}${SUFFIX}.flatpak.sha256" ]
}

_save_build_hash() {
    local arch="$1" expected="$2" current hash_file temp_file
    current="$(_compute_build_hash "${arch}")"
    if [ "${current}" != "${expected}" ]; then
        echo "WARNING: Source inputs changed during the ${arch} build; not caching this build." >&2
        return 0
    fi
    hash_file="$(_hash_file "${arch}")"
    temp_file="${hash_file}.tmp.$$"
    printf '%s\n' "${expected}" > "${temp_file}"
    mv "${temp_file}" "${hash_file}"
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

# ── Build each arch in parallel ───────────────────────────────────────────────

declare -A _BUILD_PIDS _BUILD_LOGS
_TAIL_PIDS=()

for ARCH in "${ARCHS[@]}"; do
    _LOG="${ROOT_DIR}/.flatpak-${ARCH}.log"
    _BUILD_LOGS["${ARCH}"]="${_LOG}"
    : > "${_LOG}"

    if [ "${ARCH}" = "${HOST_ARCH}" ]; then
        ( build_host_flatpak "${ARCH}" ) > "${_LOG}" 2>&1 &
    elif [ "${ARCH}" = "aarch64" ] && ${_can_cross_aarch64}; then
        ( build_cross_aarch64 ) > "${_LOG}" 2>&1 &
    else
        echo "WARNING: No build path for ${ARCH} on host ${HOST_ARCH} — skipping." >&2
        continue
    fi

    _BUILD_PIDS["${ARCH}"]=$!
    echo "==> [${ARCH}] Build started (PID ${_BUILD_PIDS["${ARCH}"]}, log: ${_LOG})"
    tail -f --pid="${_BUILD_PIDS["${ARCH}"]}" "${_LOG}" | sed -u "s/^/[${ARCH}] /" &
    _TAIL_PIDS+=($!)
done

_FAILED_ARCHS=()
for ARCH in "${!_BUILD_PIDS[@]}"; do
    if ! wait "${_BUILD_PIDS["${ARCH}"]}"; then
        _FAILED_ARCHS+=("${ARCH}")
    fi
done

for _tpid in "${_TAIL_PIDS[@]}"; do
    wait "${_tpid}" 2>/dev/null || true
done

if [ "${#_FAILED_ARCHS[@]}" -gt 0 ]; then
    for ARCH in "${_FAILED_ARCHS[@]}"; do
        echo "" >&2
        echo "==> [${ARCH}] Build FAILED. Full log: ${_BUILD_LOGS["${ARCH}"]}" >&2
    done
    exit 1
fi

fi # --no-build

mkdir -p "${WEB_ROOT}"
if [ -n "${PUBLIC_URL}" ]; then
    echo "==> Deploying install${SUFFIX}.sh (url: ${PUBLIC_URL})..."
    sed -e "s|__BUNDLE_BASE__|${PUBLIC_URL}|g" \
        -e "s|__BUNDLE_SUFFIX__|${SUFFIX}|g" \
        "${SCRIPT_DIR}/install.sh" > "${WEB_ROOT}/install${SUFFIX}.sh"
    chmod 644 "${WEB_ROOT}/install${SUFFIX}.sh"
fi

echo ""
echo "==> Done. Output in ${WEB_ROOT}:"
if ! $NO_BUILD; then
    for ARCH in "${ARCHS[@]}"; do
        echo "    repo-${ARCH}${SUFFIX}/"
        ! $BUNDLE || echo "    BioSlicer-${ARCH}${SUFFIX}.flatpak (+ .sha256)  <- GitHub Release assets"
    done
fi
if [ -n "${PUBLIC_URL}" ]; then
    echo "    install${SUFFIX}.sh"
    echo "    Install/update/launch on any machine: curl -fsSL ${PUBLIC_URL}/install${SUFFIX}.sh | bash"
fi
echo ""
if ! $NO_BUILD; then
    echo "    To install locally: flatpak/install-local.sh [arch]"
fi
