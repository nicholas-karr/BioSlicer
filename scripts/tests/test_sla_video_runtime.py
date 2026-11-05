#!/usr/bin/env python3

import base64
import hashlib
import tempfile
import unittest
from pathlib import Path

import scripts.sla_video_runtime as runtime


class ExtractVideosFromGcodeTests(unittest.TestCase):
    def test_extracts_embedded_and_reference_payloads(self):
        payload = b"fake_mkv_payload"
        payload_sha256 = hashlib.sha256(payload).hexdigest()
        payload_b64 = base64.b64encode(payload).decode("ascii")
        ref_path = "/mnt/videos/external file\nwith-newline.mkv"
        ref_path_b64 = base64.b64encode(ref_path.encode()).decode("ascii")

        gcode = "\n".join(
            [
                "; header",
                f"; bioslicer_sla_video begin name=resin_emb extruder=1 bytes={len(payload)} sha256={payload_sha256}",
                f"; bioslicer_sla_video {payload_b64}",
                "; bioslicer_sla_video end name=resin_emb",
                f"; bioslicer_sla_video ref name=resin_ref extruder=2 path_b64={ref_path_b64}",
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
            self.assertIn(payload_sha256[:12], emb_path.name)

    def test_raises_on_length_mismatch(self):
        payload = b"fake_mkv_payload"
        payload_sha256 = hashlib.sha256(payload).hexdigest()
        payload_b64 = base64.b64encode(payload).decode("ascii")
        wrong_bytes = len(payload) + 99  # intentionally wrong

        gcode = "\n".join(
            [
                f"; bioslicer_sla_video begin name=resin_bad extruder=1 bytes={wrong_bytes} sha256={payload_sha256}",
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

    def test_rejects_path_traversal_and_cleans_partial_file(self):
        payload = base64.b64encode(b"payload").decode("ascii")
        gcode = "\n".join(
            [
                "; bioslicer_sla_video begin name=../escape extruder=1 "
                "bytes=7 sha256=239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5",
                f"; bioslicer_sla_video {payload}",
                "; bioslicer_sla_video end name=../escape",
            ]
        )
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "job.gcode"
            output = Path(td) / "output"
            source.write_text(gcode, encoding="utf-8")
            with self.assertRaises(RuntimeError):
                runtime.extract_videos_from_gcode(str(source), str(output))
            self.assertFalse((Path(td) / "escape.mkv").exists())

    def test_rejects_duplicate_names(self):
        gcode = "\n".join(
            [
                "; bioslicer_sla_video ref name=resin extruder=1 path=/one.mkv",
                "; bioslicer_sla_video ref name=resin extruder=2 path=/two.mkv",
            ]
        )
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "job.gcode"
            source.write_text(gcode, encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Duplicate"):
                runtime.extract_videos_from_gcode(str(source), str(Path(td) / "out"))

    def test_streams_payload_split_at_non_base64_boundaries(self):
        payload = bytes(range(256)) * 1024
        digest = hashlib.sha256(payload).hexdigest()
        encoded = base64.b64encode(payload).decode("ascii")
        rows = [encoded[index : index + 77] for index in range(0, len(encoded), 77)]
        gcode = "\n".join(
            [
                f"; bioslicer_sla_video begin name=large extruder=1 bytes={len(payload)} sha256={digest}",
                *(f"; bioslicer_sla_video {row}" for row in rows),
                "; bioslicer_sla_video end name=large",
            ]
        )
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "job.gcode"
            source.write_text(gcode, encoding="utf-8")
            result = runtime.extract_videos_from_gcode(
                str(source), str(Path(td) / "out")
            )
            self.assertEqual(Path(result["large"]).read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
