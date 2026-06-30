#!/usr/bin/env bash
set -e

APP_ID="com.bioslicer.BioSlicer"
BUNDLE_BASE="__BUNDLE_BASE__"
BUNDLE_SUFFIX="__BUNDLE_SUFFIX__"
case "${BUNDLE_BASE}" in
    __*)
        echo "ERROR: This install.sh has not been configured for a server." >&2
        echo "       Download it from your BioSlicer server rather than running it directly from the repository." >&2
        exit 1 ;;
esac

# Select the bundle for the host CPU architecture.
_host_arch="$(uname -m)"
case "${_host_arch}" in
    aarch64|arm64) _arch=aarch64 ;;
    x86_64)        _arch=x86_64 ;;
    *)
        echo "WARNING: Unrecognized architecture '${_host_arch}', defaulting to x86_64 bundle." >&2
        _arch=x86_64 ;;
esac

# Use the arch-specific bundle.  For x86_64 only, fall back to the generic bundle if
# the arch-specific one isn't available yet.  For all other arches (e.g. aarch64)
# fail loudly rather than silently installing an incompatible x86_64 bundle.
_arch_url="${BUNDLE_BASE}/BioSlicer-${_arch}${BUNDLE_SUFFIX}.flatpak"
if curl -fsSI "${_arch_url}" &>/dev/null; then
    BUNDLE_URL="${_arch_url}"
elif [ "$_arch" = "x86_64" ]; then
    echo "NOTE: No arch-specific bundle found for '${_arch}', using generic bundle." >&2
    BUNDLE_URL="${BUNDLE_BASE}/BioSlicer${BUNDLE_SUFFIX}.flatpak"
else
    echo "ERROR: No bundle available for architecture '${_arch}' at ${_arch_url}." >&2
    echo "       Installing the generic bundle on ${_arch} would fail at runtime." >&2
    echo "       Please check ${BUNDLE_BASE} for an '${_arch}' release." >&2
    exit 1
fi

TMP_BUNDLE="$(mktemp --suffix=.flatpak)"

trap 'rm -f "$TMP_BUNDLE"' EXIT

# === Environment setup (must happen before flatpak calls) ===

# Detect WSL2
_is_wsl=false
grep -qi microsoft /proc/version 2>/dev/null && _is_wsl=true

# Fix XDG_RUNTIME_DIR.  WSLg sets it to world-writable /mnt/wslg/runtime-dir (0777)
# which D-Bus refuses.  Resolve WAYLAND_DISPLAY to an absolute path first so the
# Wayland socket is still reachable after we replace the dir.
if [ "$_is_wsl" = true ]; then
    _wslg_runtime="${XDG_RUNTIME_DIR:-/mnt/wslg/runtime-dir}"
    if [ -n "${WAYLAND_DISPLAY:-}" ] && [ "${WAYLAND_DISPLAY#/}" = "${WAYLAND_DISPLAY}" ]; then
        export WAYLAND_DISPLAY="${_wslg_runtime}/${WAYLAND_DISPLAY}"
    fi
    # Always use /run/user/$UID so that flatpak writes bwrapinfo.json to the
    # same path the portal (a system service) looks for it.  Using /tmp breaks
    # glycin's nested-sandbox check: flatpak binds /tmp/... as /run/user/$UID
    # inside the sandbox, but the portal always checks the real /run/user/$UID.
    # WSLg may point XDG_RUNTIME_DIR at a world-writable path; /run/user/$UID
    # (0700, owned by the user) satisfies D-Bus as well.
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
    sudo mkdir -p "$XDG_RUNTIME_DIR"
    sudo chown "$(id -u):$(id -g)" "$XDG_RUNTIME_DIR"
    sudo chmod 0700 "$XDG_RUNTIME_DIR"
elif [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    export XDG_RUNTIME_DIR="/tmp/$(id -u)-runtime-dir"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 0700 "$XDG_RUNTIME_DIR"
fi

# Start system D-Bus if not running.
# flatpak 1.12.x contacts org.freedesktop.MalcontentManager via the SYSTEM bus during
# both install and run; without it the call is fatal.  WSL2 without systemd has no
# system bus socket at /run/dbus/system_bus_socket.
if [ ! -S "/run/dbus/system_bus_socket" ] && command -v dbus-daemon &>/dev/null; then
    sudo mkdir -p /run/dbus
    sudo dbus-daemon --system --fork --nopidfile 2>/dev/null || true
    _w=0
    while [ ! -S "/run/dbus/system_bus_socket" ] && [ "$_w" -lt 15 ]; do
        sleep 0.2; _w=$((_w + 1))
    done
fi

# Start a session D-Bus if one is not already running.
# flatpak install --user and flatpak run both require a session bus.
# On WSL2 without systemd (and in piped curl|bash sessions) none exists by default.
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-launch &>/dev/null; then
    eval "$(dbus-launch --sh-syntax 2>/dev/null)" || true
fi

# Resolve any remaining D-Bus session socket from the runtime dir
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    for _sock in "${XDG_RUNTIME_DIR}/bus" "/run/user/$(id -u)/bus"; do
        [ -S "$_sock" ] && export DBUS_SESSION_BUS_ADDRESS="unix:path=$_sock" && break
    done
fi

# === ~/.bashrc: XDG_RUNTIME_DIR fallback block ===
_RUNTIME_BLOCK_HASH="62817c0a"
_RUNTIME_BLOCK_TAG="BIOSLICER-RUNTIME-${_RUNTIME_BLOCK_HASH}"
_RUNTIME_BLOCK="# Create a private runtime directory if it doesn't exist
if [ -z \"\$XDG_RUNTIME_DIR\" ]; then
    export XDG_RUNTIME_DIR=\"/tmp/\$(id -u)-runtime-dir\"
fi

if [ ! -d \"\$XDG_RUNTIME_DIR\" ]; then
    mkdir -p \"\$XDG_RUNTIME_DIR\"
    chmod 0700 \"\$XDG_RUNTIME_DIR\"
fi"
echo "==> Configuring XDG runtime directory in ~/.bashrc..."
if grep -qF "# BEGIN ${_RUNTIME_BLOCK_TAG}" "${HOME}/.bashrc" 2>/dev/null; then
    echo "    Already configured (${_RUNTIME_BLOCK_HASH})."
else
    if grep -q '# BEGIN BIOSLICER-RUNTIME-' "${HOME}/.bashrc" 2>/dev/null; then
        sed -i '/# BEGIN BIOSLICER-RUNTIME-/,/# END BIOSLICER-RUNTIME-/d' "${HOME}/.bashrc"
        echo "    Replaced outdated block."
    fi
    printf '\n# BEGIN %s\n%s\n# END %s\n' \
        "${_RUNTIME_BLOCK_TAG}" "${_RUNTIME_BLOCK}" "${_RUNTIME_BLOCK_TAG}" \
        >> "${HOME}/.bashrc"
    echo "    Added (${_RUNTIME_BLOCK_HASH})."
fi

# === Install flatpak if missing ===
if ! command -v flatpak &>/dev/null; then
    echo "==> Installing flatpak..."
    if command -v apt-get &>/dev/null; then
        sudo apt-get update -y
        sudo apt-get install -y --fix-missing flatpak malcontent
    elif command -v dnf &>/dev/null; then
        sudo dnf install -y flatpak malcontent
    elif command -v pacman &>/dev/null; then
        sudo pacman -S --noconfirm flatpak
    elif command -v zypper &>/dev/null; then
        sudo zypper install -y flatpak
    else
        echo "ERROR: Cannot install flatpak automatically. Please install it manually and re-run." >&2
        exit 1
    fi
fi

# flatpak 1.12.x makes a mandatory D-Bus call to org.freedesktop.MalcontentManager
# (parental controls) during both install and run; the call is fatal when the service
# is absent (common on WSL2).  1.14+ makes the check non-fatal.
# Upgrade to 1.14+ on Ubuntu/Debian via the official flatpak PPA if needed.
if command -v apt-get &>/dev/null; then
    _fp_minor=$(flatpak --version 2>/dev/null | grep -oP '1\.\K[0-9]+' | head -1)
    if [ "${_fp_minor:-99}" -lt 14 ]; then
        echo "==> Upgrading flatpak to 1.14+ (fixes WSL2 compatibility)..."
        sudo apt-get install -y --fix-missing software-properties-common 2>/dev/null || true
        sudo add-apt-repository -y ppa:flatpak/stable 2>/dev/null || true
        sudo apt-get update -y -q 2>/dev/null || true
        sudo apt-get install -y flatpak 2>/dev/null || true
    fi
fi
# Fallback: ensure malcontent is installed for systems where the upgrade didn't apply
if command -v apt-get &>/dev/null && ! dpkg -l malcontent 2>/dev/null | grep -q '^ii'; then
    sudo apt-get install -y --fix-missing malcontent 2>/dev/null || true
elif command -v dnf &>/dev/null && ! rpm -q malcontent &>/dev/null; then
    sudo dnf install -y malcontent 2>/dev/null || true
fi

# === Replace snap curl with apt curl if needed ===
# snap curl is sandboxed and can't handle certain TLS configs or pipe-through-bash installs.
_curl_path="$(command -v curl 2>/dev/null || true)"
if [ -n "$_curl_path" ] && echo "$_curl_path" | grep -q '^/snap/'; then
    echo "==> Replacing snap curl with system curl..."
    sudo snap remove curl 2>/dev/null || true
    if command -v apt-get &>/dev/null; then
        sudo apt-get install -y --fix-missing curl
    elif command -v dnf &>/dev/null; then
        sudo dnf install -y curl
    elif command -v pacman &>/dev/null; then
        sudo pacman -S --noconfirm curl
    elif command -v zypper &>/dev/null; then
        sudo zypper install -y curl
    fi
    # Reset bash's command-lookup cache so subsequent `curl` calls find /usr/bin/curl.
    hash -r
fi

echo "==> Downloading BioSlicer (${_arch})..."
BUNDLE_DATE="$(curl -fsSI "$BUNDLE_URL" 2>/dev/null | grep -i '^last-modified:' | sed 's/[Ll]ast-[Mm]odified: //' | tr -d '\r')"
[ -n "$BUNDLE_DATE" ] && echo "    Bundle built: $(TZ='America/Chicago' date -d "$BUNDLE_DATE" '+%b %d %Y %I:%M %p %Z' 2>/dev/null || echo "$BUNDLE_DATE")"
curl -fsSL "$BUNDLE_URL" -o "$TMP_BUNDLE"

echo "==> Verifying download integrity..."
EXPECTED_SHA256="$(curl -fsSL "${BUNDLE_URL}.sha256" 2>/dev/null | tr -d '[:space:]')"
if [ -n "$EXPECTED_SHA256" ]; then
    ACTUAL_SHA256="$(sha256sum "$TMP_BUNDLE" | awk '{print $1}')"
    if [ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]; then
        echo "    Checksum mismatch — retrying download..."
        curl -fsSL "$BUNDLE_URL" -o "$TMP_BUNDLE"
        ACTUAL_SHA256="$(sha256sum "$TMP_BUNDLE" | awk '{print $1}')"
        if [ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]; then
            echo "ERROR: Checksum mismatch after retry — download may be corrupted." >&2
            echo "  Expected: $EXPECTED_SHA256" >&2
            echo "  Got:      $ACTUAL_SHA256" >&2
            exit 1
        fi
    fi
    echo "    Checksum OK."
else
    echo "    (No checksum file available — skipping verification.)"
fi

echo "==> Ensuring Flathub remote is configured..."
flatpak remote-add --user --if-not-exists flathub \
    https://flathub.org/repo/flathub.flatpakrepo

echo "==> Installing BioSlicer..."
# Kill any running instances before reinstalling
if pgrep -f "flatpak run.*${APP_ID}" &>/dev/null; then
    echo "    Stopping running BioSlicer instances..."
    pkill -f "flatpak run.*${APP_ID}" 2>/dev/null || true
    sleep 0.5
fi
# Remove any previous system-wide install (from older installer versions)
sudo flatpak uninstall --system --noninteractive "$APP_ID" 2>&1 | grep -v '^$' || true
# Remove any previous user install
flatpak uninstall --user --noninteractive "$APP_ID" 2>&1 | grep -v '^$' || true
# Clean up accumulated synthetic remotes from previous bundle installs
flatpak remote-list --user 2>/dev/null \
    | awk 'NR>1 && $1 ~ /bioslicer.*-origin/ {print $1}' \
    | xargs -r flatpak remote-delete --user --force 2>/dev/null || true
# Install; --reinstall handles the case where the app is already registered
flatpak install --user --noninteractive --reinstall "$TMP_BUNDLE"

# On WSL2, allow the bioslicer wrapper to (a) create /run/user/$UID so flatpak
# and the portal agree on the bwrapinfo.json location, and (b) start the system
# D-Bus without a password prompt (needed for flatpak <1.14 malcontent check).
if [ "$_is_wsl" = true ]; then
    # Install the run-dir helper.  /run/user/$UID lives on tmpfs and disappears
    # after a WSL2 restart; the helper recreates it with 0700 ownership so
    # flatpak writes bwrapinfo.json there and the portal can find it.
    sudo tee /usr/local/sbin/bioslicer-mkrundir > /dev/null << 'MKRUNDIR_EOF'
#!/bin/sh
_uid="${1:-}"
expr "$_uid" : '^[0-9][0-9]*$' >/dev/null 2>&1 || exit 1
mkdir -p "/run/user/$_uid"
chmod 0700 "/run/user/$_uid"
chown "$_uid" "/run/user/$_uid"
MKRUNDIR_EOF
    sudo chmod 755 /usr/local/sbin/bioslicer-mkrundir

    {
        if command -v dbus-daemon &>/dev/null; then
            echo "${USER} ALL=(root) NOPASSWD: $(command -v dbus-daemon) --system --fork --nopidfile"
        fi
        echo "${USER} ALL=(root) NOPASSWD: /usr/local/sbin/bioslicer-mkrundir"
    } | sudo tee /etc/sudoers.d/bioslicer-dbus > /dev/null
    sudo chmod 440 /etc/sudoers.d/bioslicer-dbus
fi

echo "==> Installing 'bioslicer' command shortcut..."
mkdir -p "$HOME/.local/bin"
cat > "$HOME/.local/bin/bioslicer" <<'BIOSLICER_EOF'
#!/usr/bin/env bash
APP_ID="com.bioslicer.BioSlicer"

_is_wsl=false
grep -qi microsoft /proc/version 2>/dev/null && _is_wsl=true

if [ "$_is_wsl" = true ]; then
    # Pin WAYLAND_DISPLAY to an absolute path before touching XDG_RUNTIME_DIR.
    # WSLg stores the socket in its runtime dir; pin it now so the path stays
    # valid after we switch XDG_RUNTIME_DIR to /run/user/$UID.
    _wslg_runtime="${XDG_RUNTIME_DIR:-/mnt/wslg/runtime-dir}"
    if [ -n "${WAYLAND_DISPLAY:-}" ] && [ "${WAYLAND_DISPLAY#/}" = "${WAYLAND_DISPLAY}" ]; then
        export WAYLAND_DISPLAY="${_wslg_runtime}/${WAYLAND_DISPLAY}"
    fi
    # Ensure /run/user/$UID exists with the right permissions.  flatpak writes
    # bwrapinfo.json to $XDG_RUNTIME_DIR/.flatpak/<instance>/; the portal (a
    # system service) always looks for it at /run/user/$UID.  Using any other
    # path (/tmp, /mnt/wslg/...) causes a portal AccessDenied which makes
    # glycin fatal-assert GTK on the very first SVG icon load.
    # /run/user/$UID is on tmpfs and disappears after a WSL2 restart, so the
    # sudoers-allowed helper recreates it when needed.
    sudo -n /usr/local/sbin/bioslicer-mkrundir "$(id -u)" 2>/dev/null || true
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
    # Start system D-Bus if not running (needed for flatpak <1.14 malcontent check).
    # sudoers rule installed by bioslicer installer allows this without a password.
    if [ ! -S "/run/dbus/system_bus_socket" ] && command -v dbus-daemon &>/dev/null; then
        sudo -n dbus-daemon --system --fork --nopidfile 2>/dev/null || true
    fi
    # Start session D-Bus if not running.
    if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-launch &>/dev/null; then
        eval "$(dbus-launch --sh-syntax 2>/dev/null)" || true
    fi
fi

exec flatpak run "$APP_ID" "$@"
BIOSLICER_EOF
chmod +x "$HOME/.local/bin/bioslicer"
case ":${PATH}:" in
    *":$HOME/.local/bin:"*) ;;
    *) echo "    NOTE: Add $HOME/.local/bin to your PATH to use the 'bioslicer' command." ;;
esac

echo "==> Launching BioSlicer..."

if [ "$_is_wsl" = true ]; then
    echo "    WSL2 — launching (Ctrl-C to quit)..."
    flatpak run "$APP_ID"
elif command -v systemd-run &>/dev/null && systemctl --user is-active graphical-session.target &>/dev/null; then
    echo "    Launching via systemd user session (Ctrl-C to quit)..."
    systemd-run --user --wait flatpak run "$APP_ID"
elif [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    echo "    Launching (Ctrl-C to quit)..."
    flatpak run "$APP_ID"
else
    echo "    Installation complete. Launch BioSlicer from your app menu or run: bioslicer"
fi
