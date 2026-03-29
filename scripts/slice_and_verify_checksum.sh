#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLICER_BIN_DEFAULT="$ROOT_DIR/build/src/prusa-slicer"
SLICER_BIN="${SLICER_BIN:-$SLICER_BIN_DEFAULT}"

if [[ ! -x "$SLICER_BIN" ]]; then
  if command -v prusa-slicer >/dev/null 2>&1; then
    SLICER_BIN="$(command -v prusa-slicer)"
  else
    echo "Error: slicer binary not found. Expected '$SLICER_BIN_DEFAULT' or 'prusa-slicer' in PATH." >&2
    exit 1
  fi
fi

INPUT_STL="${1:-$ROOT_DIR/resources/shapes/3DBenchy.stl}"
CONFIG_INI="${CONFIG_INI:-$ROOT_DIR/tests/data/default_fff.ini}"
OUT_GCODE="${OUT_GCODE:-$ROOT_DIR/build/checksum_verify_output.gcode}"

if [[ ! -f "$INPUT_STL" ]]; then
  echo "Error: input STL not found: $INPUT_STL" >&2
  exit 1
fi

if [[ ! -f "$CONFIG_INI" ]]; then
  echo "Error: config file not found: $CONFIG_INI" >&2
  exit 1
fi

mkdir -p "$(dirname "$OUT_GCODE")"

echo "Slicing: $INPUT_STL"
"$SLICER_BIN" \
  --load "$CONFIG_INI" \
  --export-gcode \
  --output "$OUT_GCODE" \
  "$INPUT_STL"

echo "Verifying checksum in: $OUT_GCODE"
python3 - "$OUT_GCODE" <<'PY'
import hashlib
import re
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()

first_nl = data.find(b"\n")
if first_nl < 0:
    raise SystemExit("FAIL: missing first newline")

second_nl = data.find(b"\n", first_nl + 1)
if second_nl < 0:
    raise SystemExit("FAIL: missing second newline")

line2_raw = data[first_nl + 1:second_nl].decode("ascii", errors="strict")
m = re.fullmatch(r"; SHA256: ([0-9a-f]{64})", line2_raw)
if not m:
    raise SystemExit(f"FAIL: invalid checksum line format: {line2_raw!r}")

expected = m.group(1)
payload = data[second_nl + 1 :]
actual = hashlib.sha256(payload).hexdigest()

if actual != expected:
    raise SystemExit(
        "FAIL: checksum mismatch\n"
        f"expected: {expected}\n"
        f"actual:   {actual}"
    )

print("PASS: checksum is valid")
print(f"checksum: {actual}")
PY

echo "Done"