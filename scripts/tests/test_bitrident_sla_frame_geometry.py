#!/usr/bin/env python3
"""End-to-end geometry test for the BioTrident 250 SLA channel.

A 10 × 10 mm square box is placed at the exact centre of the BioTrident 250
projector area.  After slicing and embedding the SLA video, one frame is
decoded and two invariants are checked:

  1. **Percentage** – lit pixels are ~1 % of the frame area
     (10 × 10 mm square inside a 100 × 100 mm projector window).

  2. **Centring** – the centroid of lit pixels is within 1 pixel of the
     geometric frame centre (the shape is centred on the projector, so the
     centroid must be too).

Config values (frame size, projector dimensions, bed shape) are read directly
from the real ``resources/profiles/UTDHBL.ini`` so that any future edit to
that file immediately affects this test.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SLICER_BIN = REPO_ROOT / "build" / "src" / "prusa-slicer"
UTDHBL_INI = REPO_ROOT / "resources" / "profiles" / "UTDHBL.ini"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import sla_video_runtime as runtime
from hybrid_3mf_utils import (
    MeshObject,
    bed_shape_bbox,
    make_box,
    parse_vendor_ini_section,
    run_slice,
    write_3mf,
    write_sla_override_ini,
)


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------

def proj_bbox(
    bed_min_x: float, bed_min_y: float, bed_max_x: float, bed_max_y: float,
    proj_w_mm: float, proj_h_mm: float,
) -> tuple[float, float, float, float]:
    """Mirror the C++ sla_proj_bbox logic: explicit dims or max-bed-dim square."""
    cx = (bed_min_x + bed_max_x) / 2.0
    cy = (bed_min_y + bed_max_y) / 2.0
    if proj_w_mm > 0.0 and proj_h_mm > 0.0:
        hw, hh = proj_w_mm / 2.0, proj_h_mm / 2.0
    else:
        d = max(bed_max_x - bed_min_x, bed_max_y - bed_min_y) / 2.0
        hw, hh = d, d
    return cx - hw, cy - hh, cx + hw, cy + hh


# ---------------------------------------------------------------------------
# Frame decode helper
# ---------------------------------------------------------------------------

def decode_first_frame_raw(video_path: Path, frame_w: int, frame_h: int) -> bytes:
    result = subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-i", str(video_path),
            "-frames:v", "1",
            "-pix_fmt", "gray",
            "-f", "rawvideo",
            "pipe:1",
        ],
        capture_output=True,
        check=True,
    )
    raw = result.stdout
    expected = frame_w * frame_h
    if len(raw) != expected:
        raise AssertionError(
            f"Expected {expected} bytes from ffmpeg, got {len(raw)}"
        )
    return raw


# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------

class BioTridentSLAFrameGeometryTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls) -> None:
        if not SLICER_BIN.exists():
            raise AssertionError(f"Slicer binary not found: {SLICER_BIN} — build first")
        if not UTDHBL_INI.exists():
            raise AssertionError(f"UTDHBL.ini not found: {UTDHBL_INI}")

        kv = parse_vendor_ini_section(UTDHBL_INI, "common_bioslicer_trident")

        cls.frame_w  = int(kv["sla_material_video_synth_width"])
        cls.frame_h  = int(kv["sla_material_video_synth_height"])
        cls.proj_w   = float(kv["sla_material_video_synth_proj_width"])
        cls.proj_h   = float(kv["sla_material_video_synth_proj_height"])
        cls.bed_shape_str = kv["bed_shape"]

        bed_min_x, bed_min_y, bed_max_x, bed_max_y = bed_shape_bbox(cls.bed_shape_str)
        pmin_x, pmin_y, pmax_x, pmax_y = proj_bbox(
            bed_min_x, bed_min_y, bed_max_x, bed_max_y,
            cls.proj_w, cls.proj_h,
        )
        cls.proj_center_x = (pmin_x + pmax_x) / 2.0
        cls.proj_center_y = (pmin_y + pmax_y) / 2.0
        cls.proj_span_x   = pmax_x - pmin_x
        cls.proj_span_y   = pmax_y - pmin_y

    def test_percentage_and_centring(self) -> None:
        """A 10 × 10 mm square centred on the projector must expose ~1 % of
        the frame with its centroid at the frame centre."""

        SIDE_MM = 10.0

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)

            cx, cy = self.proj_center_x, self.proj_center_y
            verts, tris = make_box(
                cx - SIDE_MM / 2, cy - SIDE_MM / 2, 0.0,
                cx + SIDE_MM / 2, cy + SIDE_MM / 2, 2.0,
            )
            obj = MeshObject(
                name="centred_square_SLA",
                extruder=1,
                vertices=verts,
                triangles=tris,
            )

            path_3mf = tmp / "centred_square.3mf"
            write_3mf(path_3mf, [obj], model_name="centred_square")

            path_ini = tmp / "centred_square.ini"
            write_sla_override_ini(
                path_ini,
                sla_flags=[1],
                video_names=["square"],
                synth_flags=[1],
                embed_flags=[1],
                video_paths=[""],
                synth_width=self.frame_w,
                synth_height=self.frame_h,
                synth_fps=5,
                synth_lossless=True,
            )

            path_gcode = tmp / "centred_square.gcode"
            run_slice(
                SLICER_BIN,
                path_3mf,
                path_gcode,
                path_ini,
                extruder_count=1,
                layer_height=1.0,
                start_note="BioTrident geometry test",
                bed_shape=self.bed_shape_str,
            )

            extracted = runtime.extract_videos_from_gcode(
                str(path_gcode), str(tmp / "video_cache")
            )
            self.assertIn("square", extracted, "SLA video 'square' not found in gcode")
            path_video = Path(extracted["square"])
            self.assertTrue(path_video.exists())

            raw = decode_first_frame_raw(path_video, self.frame_w, self.frame_h)

        # -- Pixel analysis -------------------------------------------------
        total_px = self.frame_w * self.frame_h
        lit_px   = sum(1 for b in raw if b >= 128)
        pct_lit  = 100.0 * lit_px / total_px

        expected_pct = 100.0 * (SIDE_MM / self.proj_span_x) * (SIDE_MM / self.proj_span_y)

        self.assertAlmostEqual(
            pct_lit, expected_pct, delta=0.1,
            msg=(
                f"Lit pixel fraction {pct_lit:.4f} % differs from expected "
                f"{expected_pct:.4f} % by more than 0.1 pp "
                f"(frame {self.frame_w}×{self.frame_h}, "
                f"projector {self.proj_span_x}×{self.proj_span_y} mm, "
                f"shape {SIDE_MM}×{SIDE_MM} mm)"
            ),
        )

        # Centroid of lit pixels must be within 1 pixel of the frame centre.
        cx_sum = cy_sum = 0.0
        for idx, b in enumerate(raw):
            if b >= 128:
                row = idx // self.frame_w
                col = idx  % self.frame_w
                cy_sum += row
                cx_sum += col

        centroid_col = cx_sum / lit_px
        centroid_row = cy_sum / lit_px
        frame_centre_col = (self.frame_w - 1) / 2.0
        frame_centre_row = (self.frame_h - 1) / 2.0

        self.assertAlmostEqual(
            centroid_col, frame_centre_col, delta=1.0,
            msg=(
                f"Centroid column {centroid_col:.2f} is more than 1 px from "
                f"frame centre {frame_centre_col:.2f}"
            ),
        )
        self.assertAlmostEqual(
            centroid_row, frame_centre_row, delta=1.0,
            msg=(
                f"Centroid row {centroid_row:.2f} is more than 1 px from "
                f"frame centre {frame_centre_row:.2f}"
            ),
        )


if __name__ == "__main__":
    unittest.main()
