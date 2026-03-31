#!/usr/bin/env python3

import base64
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import scripts.sla_video_runtime as runtime


class DummyStream:
    instances = []

    def __init__(self, name, path, hwaccel="auto", hw_decoder=None):
        self.name = name
        self.path = path
        self.hwaccel = hwaccel
        self.hw_decoder = hw_decoder
        self.closed = False
        self.current_frame = 0
        self.metadata = runtime.VideoMetadata(
            name=name,
            path=path,
            width=64,
            height=32,
            fps=24.0,
            frame_count=100,
            codec="hevc",
        )
        DummyStream.instances.append(self)

    def close(self):
        self.closed = True

    def get_frame(self, frame_index):
        self.current_frame = frame_index + 1
        return b"\x00" * (self.metadata.width * self.metadata.height * 4)

    def to_dict(self):
        return {
            "name": self.metadata.name,
            "path": self.metadata.path,
            "width": self.metadata.width,
            "height": self.metadata.height,
            "fps": self.metadata.fps,
            "frame_count": self.metadata.frame_count,
            "codec": self.metadata.codec,
            "current_frame": self.current_frame,
            "hwaccel": self.hwaccel,
            "hw_decoder": self.hw_decoder,
        }


class ExtractVideosFromGcodeTests(unittest.TestCase):
    def test_extracts_embedded_and_reference_payloads(self):
        payload = b"fake_mkv_payload"
        payload_b64 = base64.b64encode(payload).decode("ascii")
        payload_sha = hashlib.sha256(payload).hexdigest()
        ref_path = "/mnt/videos/external.mkv"

        gcode = "\n".join(
            [
                "; header",
                f"; bioslicer_sla_video begin name=resin_emb extruder=1 bytes={len(payload)} sha256={payload_sha}",
                f"; bioslicer_sla_video {payload_b64}",
                "; bioslicer_sla_video end name=resin_emb",
                f"; bioslicer_sla_video ref name=resin_ref extruder=2 path={ref_path}",
            ]
        )

        with tempfile.TemporaryDirectory() as td:
            gcode_path = Path(td) / "job.gcode"
            out_dir = Path(td) / "cache"
            gcode_path.write_text(gcode, encoding="utf-8")

            extracted = runtime.extract_videos_from_gcode(str(gcode_path), str(out_dir))

            self.assertEqual(extracted["resin_ref"], ref_path)
            emb_path = Path(extracted["resin_emb"])
            self.assertTrue(emb_path.exists())
            self.assertEqual(emb_path.read_bytes(), payload)

    def test_raises_on_digest_mismatch(self):
        payload = b"fake_mkv_payload"
        payload_b64 = base64.b64encode(payload).decode("ascii")
        bad_sha = "0" * 64

        gcode = "\n".join(
            [
                f"; bioslicer_sla_video begin name=resin_bad extruder=1 bytes={len(payload)} sha256={bad_sha}",
                f"; bioslicer_sla_video {payload_b64}",
                "; bioslicer_sla_video end name=resin_bad",
            ]
        )

        with tempfile.TemporaryDirectory() as td:
            gcode_path = Path(td) / "job.gcode"
            out_dir = Path(td) / "cache"
            gcode_path.write_text(gcode, encoding="utf-8")

            with self.assertRaises(RuntimeError):
                runtime.extract_videos_from_gcode(str(gcode_path), str(out_dir))


class VideoRegistryTests(unittest.TestCase):
    def setUp(self):
        DummyStream.instances = []

    def test_load_list_get_and_unload_video(self):
        with tempfile.TemporaryDirectory() as td, \
            mock.patch.object(runtime, "FFmpegVideoStream", DummyStream), \
            mock.patch.object(runtime.os.path, "isfile", return_value=True):
            registry = runtime.VideoRegistry(cache_dir=td, hwaccel="cuda", hw_decoder="hevc_cuvid")

            meta = registry.load_video("resin_a", "/tmp/resin_a.mkv")
            self.assertEqual(meta["name"], "resin_a")
            self.assertEqual(meta["hwaccel"], "cuda")

            videos = registry.list_videos()
            self.assertIn("resin_a", videos)

            width, height, frame = registry.get_frame("resin_a", 3)
            self.assertEqual((width, height), (64, 32))
            self.assertEqual(len(frame), 64 * 32 * 4)

            self.assertTrue(registry.unload_video("resin_a"))
            self.assertFalse(registry.unload_video("resin_a"))

    def test_overwriting_video_closes_previous_stream(self):
        with tempfile.TemporaryDirectory() as td, \
            mock.patch.object(runtime, "FFmpegVideoStream", DummyStream), \
            mock.patch.object(runtime.os.path, "isfile", return_value=True):
            registry = runtime.VideoRegistry(cache_dir=td)
            registry.load_video("resin", "/tmp/a.mkv")
            first = DummyStream.instances[0]

            registry.load_video("resin", "/tmp/b.mkv")
            self.assertTrue(first.closed)
            self.assertEqual(registry.videos["resin"].path, "/tmp/b.mkv")

    def test_load_videos_from_gcode_delegates_to_load_video(self):
        extracted = {"a": "/tmp/a.mkv", "b": "/tmp/b.mkv"}

        with tempfile.TemporaryDirectory() as td, \
            mock.patch.object(runtime.os.path, "isfile", return_value=True), \
            mock.patch.object(runtime, "extract_videos_from_gcode", return_value=extracted), \
            mock.patch.object(runtime.VideoRegistry, "load_video", side_effect=lambda name, path: {"name": name, "path": path}) as load_video_mock:
            registry = runtime.VideoRegistry(cache_dir=td)
            loaded = registry.load_videos_from_gcode("/tmp/job.gcode")

            self.assertEqual(loaded["a"]["path"], "/tmp/a.mkv")
            self.assertEqual(loaded["b"]["path"], "/tmp/b.mkv")
            self.assertEqual(load_video_mock.call_count, 2)


if __name__ == "__main__":
    unittest.main()
