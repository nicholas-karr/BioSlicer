#!/usr/bin/env python3
"""End-to-end test: slice an SLA-only pyramid, decode embedded video, assert decreasing exposure area per frame."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hybrid_3mf_utils import MeshObject, write_3mf, write_sla_override_ini, run_slice
import sla_video_runtime

REPO_ROOT = Path(__file__).resolve().parents[2]
SLICER_BIN = REPO_ROOT / "build" / "src" / "prusa-slicer"


def make_pyramid(cx, cy, base_size, height):
    h = base_size / 2.0
    verts = [
        (cx - h, cy - h, 0.0),
        (cx + h, cy - h, 0.0),
        (cx + h, cy + h, 0.0),
        (cx - h, cy + h, 0.0),
        (cx,     cy,     height),
    ]
    tris = [
        (0, 2, 1), (0, 3, 2),
        (0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4),
    ]
    return verts, tris


def ffprobe_frame_count(video_path):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
         "-show_entries", "stream=nb_read_frames", "-of", "default=nokey=1:noprint_wrappers=1",
         str(video_path)],
        capture_output=True, text=True, check=True,
    )
    return int(result.stdout.strip())


def extract_frame(video_path, frame_idx, out_png):
    subprocess.run(
        ["ffmpeg", "-y", "-i", str(video_path),
         "-vf", f"select=eq(n\\,{frame_idx})", "-frames:v", "1", "-vsync", "vfr", str(out_png)],
        capture_output=True, check=True,
    )


def count_white_pixels(png_path, threshold=128):
    from PIL import Image
    img = Image.open(png_path).convert("L")
    return sum(1 for p in img.getdata() if p >= threshold)


class SlaPyramidTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SLICER_BIN.exists():
            raise unittest.SkipTest(f"Missing slicer binary: {SLICER_BIN}")
        try:
            from PIL import Image  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("Pillow (PIL) not installed")
        result = subprocess.run(["ffmpeg", "-version"], capture_output=True)
        if result.returncode != 0:
            raise unittest.SkipTest("ffmpeg not available")

    def test_pyramid_exposure_decreases(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            verts, tris = make_pyramid(cx=50.0, cy=50.0, base_size=40.0, height=20.0)
            obj = MeshObject(name="pyramid_SLA", extruder=1, vertices=verts, triangles=tris)

            path_3mf = tmp / "pyramid_sla.3mf"
            write_3mf(path_3mf, [obj], model_name="sla_pyramid")

            path_ini = tmp / "pyramid_sla.ini"
            write_sla_override_ini(
                path_ini,
                sla_flags=[1],
                video_names=["pyramid"],
                synth_flags=[1],
                embed_flags=[1],
                video_paths=[""],
                synth_width=320,
                synth_height=240,
                synth_fps=5,
                synth_lossless=True,
            )

            path_gcode = tmp / "pyramid_sla.gcode"
            run_slice(SLICER_BIN, path_3mf, path_gcode, path_ini,
                      extruder_count=1, layer_height=1.0, start_note="SLA pyramid test")
            self.assertTrue(path_gcode.exists(), "slicer produced no gcode output")

            extracted = sla_video_runtime.extract_videos_from_gcode(str(path_gcode), str(tmp / "video_extract"))
            self.assertIn("pyramid", extracted, f"no 'pyramid' video found; got: {list(extracted)}")
            path_video = Path(extracted["pyramid"])

            n_frames = ffprobe_frame_count(path_video)
            self.assertGreaterEqual(n_frames, 3, f"expected ≥3 frames, got {n_frames}")

            frame_indices = [0, n_frames // 2, n_frames - 1]
            white_counts = []
            for fi in frame_indices:
                png = tmp / f"frame_{fi:03d}.png"
                extract_frame(path_video, fi, png)
                white_counts.append(count_white_pixels(png))

            self.assertGreater(white_counts[0], white_counts[1],
                f"frame {frame_indices[0]} ({white_counts[0]}) should have more white pixels than frame {frame_indices[1]} ({white_counts[1]})")
            self.assertGreater(white_counts[1], white_counts[2],
                f"frame {frame_indices[1]} ({white_counts[1]}) should have more white pixels than frame {frame_indices[2]} ({white_counts[2]})")


if __name__ == "__main__":
    unittest.main()
