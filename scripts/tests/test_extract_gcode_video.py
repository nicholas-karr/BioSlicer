#!/usr/bin/env python3
"""Unit tests for extract_gcode_video.py."""

import base64
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parents[1]
SCRIPT = SCRIPTS_DIR / "extract_gcode_video.py"

VIDEO_DATA = bytes(range(256)) * 4 + b"\x1a\x45\xdf\xa3"  # 1028 bytes


def make_gcode(video_data: bytes, name: str = "sla_ch1", extruder: int = 0) -> str:
    encoded = base64.b64encode(video_data).decode()
    max_row = 78
    lines = [
        "; BioSlicer generated",
        f"; bioslicer_sla_material_map extruder={extruder} name={name} embedded=1",
        f"; bioslicer_sla_video_begin name={name} extruder={extruder} bytes={len(video_data)}",
    ]
    for offset in range(0, len(encoded), max_row):
        lines.append(f"; bioslicer_sla_video {encoded[offset:offset + max_row]}")
    lines.append(f"; bioslicer_sla_video end name={name}")
    lines.append("")
    lines.append("G28 ; home")
    return "\n".join(lines)


def run_extract(gcode: str, name: str = "sla_ch1", extra_args=None):
    if extra_args is None:
        extra_args = []
    with tempfile.NamedTemporaryFile(suffix=".gcode", mode="w", delete=False) as f:
        f.write(gcode)
        gcode_path = Path(f.name)
    out_path = gcode_path.with_name(f"{name}.mkv")
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(gcode_path)] + extra_args,
        capture_output=True, text=True,
    )
    return out_path, result, gcode_path


class ExtractGcodeVideoTests(unittest.TestCase):
    def test_basic_extraction(self):
        gcode = make_gcode(VIDEO_DATA)
        out, result, gcode_path = run_extract(gcode)
        try:
            self.assertEqual(result.returncode, 0, f"non-zero exit\n{result.stderr}")
            self.assertTrue(out.exists(), f"output not created at {out}")
            self.assertEqual(out.read_bytes(), VIDEO_DATA, "payload mismatch")
        finally:
            gcode_path.unlink(missing_ok=True)
            out.unlink(missing_ok=True)

    def test_explicit_output(self):
        with tempfile.NamedTemporaryFile(suffix=".mkv", delete=False) as f:
            explicit_out = Path(f.name)
        with tempfile.NamedTemporaryFile(suffix=".gcode", mode="w", delete=False) as f:
            f.write(make_gcode(VIDEO_DATA))
            gcode_path = Path(f.name)
        try:
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(gcode_path), "-o", str(explicit_out)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, f"non-zero exit\n{result.stderr}")
            self.assertEqual(explicit_out.read_bytes(), VIDEO_DATA, "payload mismatch")
        finally:
            gcode_path.unlink(missing_ok=True)
            explicit_out.unlink(missing_ok=True)

    def test_named_extraction(self):
        gcode = make_gcode(VIDEO_DATA, name="ch1", extruder=0)
        video2 = bytes(reversed(VIDEO_DATA))
        gcode += "\n" + make_gcode(video2, name="ch2", extruder=1)

        with tempfile.NamedTemporaryFile(suffix=".gcode", mode="w", delete=False) as f:
            f.write(gcode)
            gcode_path = Path(f.name)
        out_path = gcode_path.with_name("ch2.mkv")
        try:
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(gcode_path), "-n", "ch2"],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, f"non-zero exit\n{result.stderr}")
            self.assertEqual(out_path.read_bytes(), video2, "payload mismatch for ch2")
        finally:
            gcode_path.unlink(missing_ok=True)
            out_path.unlink(missing_ok=True)

    def test_no_video(self):
        gcode = "; BioSlicer generated\nG28 ; home\n"
        with tempfile.NamedTemporaryFile(suffix=".gcode", mode="w", delete=False) as f:
            f.write(gcode)
            gcode_path = Path(f.name)
        try:
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(gcode_path)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0, "should have exited non-zero")
            self.assertIn("No embedded video", result.stderr)
        finally:
            gcode_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
