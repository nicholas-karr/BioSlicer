#!/usr/bin/env python3

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import scripts.sla_video_runtime as runtime


def _probe_video_size(video_path: Path) -> tuple[int, int]:
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height",
            "-of",
            "csv=p=0:s=x",
            str(video_path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    width_str, height_str = probe.stdout.strip().split("x", 1)
    return int(width_str), int(height_str)


def _probe_video_frame_count(video_path: Path) -> int:
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=avg_frame_rate:format=duration",
            "-of",
            "default=noprint_wrappers=1",
            str(video_path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    fps = 0.0
    duration = 0.0
    for line in probe.stdout.splitlines():
        if line.startswith("avg_frame_rate="):
            num, den = line.split("=", 1)[1].split("/", 1)
            num_f = float(num)
            den_f = float(den)
            if den_f > 0:
                fps = num_f / den_f
        elif line.startswith("duration="):
            duration = float(line.split("=", 1)[1])

    if fps <= 0 or duration <= 0:
        return 1
    return max(1, int(round(fps * duration)))


def _decode_gray_frame(video_path: Path, frame_index: int, width: int, height: int) -> bytes:
    decoded = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(video_path),
            "-vf",
            f"select=eq(n\\,{frame_index})",
            "-vframes",
            "1",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "gray",
            "-",
        ],
        check=True,
        capture_output=True,
    )
    expected_size = width * height
    if len(decoded.stdout) != expected_size:
        raise AssertionError(
            f"Decoded frame {frame_index} has unexpected size: "
            f"expected {expected_size}, got {len(decoded.stdout)}"
        )
    return decoded.stdout


def _line_orientation_metrics(gray_frame: bytes, width: int, height: int) -> tuple[int, int]:
    row_peak = 0
    for y in range(height):
        row_sum = 0
        offset = y * width
        for x in range(width):
            row_sum += gray_frame[offset + x]
        row_peak = max(row_peak, row_sum)

    col_peak = 0
    for x in range(width):
        col_sum = 0
        for y in range(height):
            col_sum += gray_frame[y * width + x]
        col_peak = max(col_peak, col_sum)

    return row_peak, col_peak


def _lit_bbox(gray_frame: bytes, width: int, height: int) -> tuple[int, int, int, int] | None:
    min_x = width
    max_x = -1
    min_y = height
    max_y = -1

    for y in range(height):
        row_off = y * width
        for x in range(width):
            if gray_frame[row_off + x] > 0:
                min_x = min(min_x, x)
                max_x = max(max_x, x)
                min_y = min(min_y, y)
                max_y = max(max_y, y)

    if max_x < 0:
        return None
    return min_x, max_x, min_y, max_y


class HybridScriptSmokeTests(unittest.TestCase):
    def test_native_synthesis_smoke(self):
        repo_root = Path(__file__).resolve().parents[2]
        script_path = repo_root / "scripts" / "gen_hybrid_3mf_and_slice.py"
        slicer_path = repo_root / "build" / "src" / "prusa-slicer"

        if not slicer_path.exists():
            self.skipTest(f"Missing slicer binary: {slicer_path}")

        if shutil.which("ffmpeg") is None:
            raise AssertionError("ffmpeg is required for native synthesis smoke test")

        with tempfile.TemporaryDirectory(prefix="bioslicer_hybrid_smoke_") as td:
            out_dir = Path(td)
            job_name = "hybrid_native_smoke"
            self.assertTrue(script_path.exists(), f"Missing script: {script_path}")

            subprocess.run(
                [
                    "python3",
                    str(script_path),
                    "--prusa-slicer",
                    str(slicer_path),
                    "--output-dir",
                    str(out_dir),
                    "--name",
                    job_name,
                    "--sla-synth-width",
                    "640",
                    "--sla-synth-height",
                    "360",
                    "--sla-synth-fps",
                    "6",
                ],
                check=True,
                cwd=repo_root,
            )

            gcode_path = out_dir / f"{job_name}.gcode"
            override_path = out_dir / f"{job_name}_sla_override.ini"

            self.assertTrue(gcode_path.exists(), f"Missing output gcode: {gcode_path}")
            self.assertTrue(override_path.exists(), f"Missing override ini: {override_path}")

            override_text = override_path.read_text(encoding="utf-8")
            self.assertIn("sla_material_video_synthesize = 0,1", override_text)

            gcode_text = gcode_path.read_text(encoding="utf-8", errors="replace")
            # Validate FDM content: at least one extrusion move with an E axis value.
            self.assertRegex(gcode_text, re.compile(r"^G1\s+.*\bE[-+]?(?:\d|\.\d)", re.MULTILINE))
            self.assertIn("; bioslicer_sla_material_map extruder=1 name=resin_basic embedded=1", gcode_text)
            self.assertIn("; bioslicer_sla_video begin name=resin_basic extruder=1", gcode_text)
            self.assertIn("; bioslicer_sla_video end name=resin_basic", gcode_text)

            extracted = runtime.extract_videos_from_gcode(str(gcode_path), str(out_dir / "video_extract"))
            self.assertIn("resin_basic", extracted)
            video_path = Path(extracted["resin_basic"])
            self.assertTrue(video_path.exists(), f"Missing extracted video: {video_path}")

            width, height = _probe_video_size(video_path)
            self.assertEqual((width, height), (640, 360))
            frame_count = _probe_video_frame_count(video_path)

            frame0 = _decode_gray_frame(video_path, 0, width, height)
            frame_mid_idx = min(frame_count - 1, max(1, frame_count // 2))
            frame_last_idx = frame_count - 1
            frame_mid = _decode_gray_frame(video_path, frame_mid_idx, width, height)
            frame_last = _decode_gray_frame(video_path, frame_last_idx, width, height)

            row_peak_0, col_peak_0 = _line_orientation_metrics(frame0, width, height)
            row_peak_mid, col_peak_mid = _line_orientation_metrics(frame_mid, width, height)
            row_peak_last, col_peak_last = _line_orientation_metrics(frame_last, width, height)

            # Input model alternates orientation each layer: X-lines then Y-lines.
            self.assertGreater(row_peak_0, col_peak_0 * 4)
            self.assertGreater(col_peak_mid, row_peak_mid * 4)
            self.assertGreater(row_peak_last, col_peak_last * 4)

            # Model spans 60 mm of a 250 mm bed, so footprint should be much smaller than full frame.
            bbox = _lit_bbox(frame0, width, height)
            self.assertIsNotNone(bbox)
            min_x, max_x, min_y, max_y = bbox
            lit_w = max_x - min_x + 1
            lit_h = max_y - min_y + 1
            self.assertLess(lit_w / width, 0.35)
            self.assertLess(lit_h / height, 0.35)


if __name__ == "__main__":
    unittest.main()
