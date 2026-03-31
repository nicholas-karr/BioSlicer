#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="${SERVICE_NAME:-bioslicer-image-display.service}"
SERVICE_USER="${SERVICE_USER:-pi}"
SCRIPTS_DIR="${SCRIPTS_DIR:-/home/pi/BioSlicer/scripts}"
CONFIG_PATH="${CONFIG_PATH:-${SCRIPTS_DIR}/image-display-config.yaml}"
DISPLAY_VALUE="${DISPLAY_VALUE:-:0}"
XAUTHORITY_PATH="${XAUTHORITY_PATH:-/home/${SERVICE_USER}/.Xauthority}"
PYTHON_BIN="${PYTHON_BIN:-${SCRIPTS_DIR}/.venv/bin/python3}"

if [[ "${EUID}" -ne 0 ]]; then
    echo "Run as root: sudo $0"
    exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "systemctl is not available on this host. Install/start image-display.py manually."
    exit 1
fi

if [[ ! -f "${SCRIPTS_DIR}/image-display.py" ]]; then
    echo "image-display.py not found in ${SCRIPTS_DIR}"
    exit 1
fi

if [[ ! -f "${CONFIG_PATH}" ]]; then
    echo "Config not found: ${CONFIG_PATH}"
    echo "Create one from template first:"
    echo "  cp ${SCRIPTS_DIR}/image-display-config.yaml.template ${CONFIG_PATH}"
    exit 1
fi

if [[ ! -x "${PYTHON_BIN}" ]]; then
    PYTHON_BIN="/usr/bin/python3"
fi

if [[ ! -x "${PYTHON_BIN}" ]]; then
    echo "Python executable not found: ${PYTHON_BIN}"
    exit 1
fi

CACHE_DIR="/home/${SERVICE_USER}/printer_data/cache/bioslicer-sla-video-cache"
mkdir -p "${CACHE_DIR}" || true
chown "${SERVICE_USER}:${SERVICE_USER}" "${CACHE_DIR}" || true

UNIT_PATH="/etc/systemd/system/${SERVICE_NAME}"
cat >"${UNIT_PATH}" <<EOF
[Unit]
Description=BioSlicer image display server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${SERVICE_USER}
Group=${SERVICE_USER}
WorkingDirectory=${SCRIPTS_DIR}
Environment=PYTHONUNBUFFERED=1
Environment=DISPLAY=${DISPLAY_VALUE}
Environment=XAUTHORITY=${XAUTHORITY_PATH}
ExecStart=${PYTHON_BIN} ${SCRIPTS_DIR}/image-display.py ${CONFIG_PATH}
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now "${SERVICE_NAME}"
systemctl --no-pager --full status "${SERVICE_NAME}" || true

echo
echo "Installed and started ${SERVICE_NAME}"
echo "If your display server is not on :0, rerun with DISPLAY_VALUE set, for example:"
echo "  sudo DISPLAY_VALUE=:1 $0"
