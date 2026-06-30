#!/usr/bin/env python3
"""Unit tests for hybrid_3mf_utils helpers.

Tests here cover pure Python logic with no external dependencies (no slicer
binary, no ffmpeg, no real ini files).
"""

from __future__ import annotations

import io
import tempfile
import unittest
import zipfile
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from hybrid_3mf_utils import (
    bed_shape_bbox,
    make_box,
    make_model_config_xml,
    MeshObject,
    parse_vendor_ini_section,
    write_3mf,
    write_slice_settings_ini,
    write_sla_override_ini,
)


# ---------------------------------------------------------------------------
# parse_vendor_ini_section
# ---------------------------------------------------------------------------

class ParseVendorIniSectionTests(unittest.TestCase):
    def _write_ini(self, content: str) -> Path:
        tmp = Path(tempfile.mktemp(suffix=".ini"))
        tmp.write_text(content, encoding="utf-8")
        self.addCleanup(tmp.unlink, missing_ok=True)
        return tmp

    def test_reads_keys_from_named_section(self):
        ini = self._write_ini(
            "[printer:*common_bioslicer_trident*]\n"
            "bed_shape = 77x79,177x79,177x134,77x134\n"
            "extruders_count = 8\n"
        )
        kv = parse_vendor_ini_section(ini, "common_bioslicer_trident")
        self.assertEqual(kv["bed_shape"], "77x79,177x79,177x134,77x134")
        self.assertEqual(kv["extruders_count"], "8")

    def test_returns_empty_dict_for_missing_section(self):
        ini = self._write_ini("[printer:other_printer]\nfoo = bar\n")
        kv = parse_vendor_ini_section(ini, "missing_section")
        self.assertEqual(kv, {})

    def test_stops_at_next_section(self):
        ini = self._write_ini(
            "[printer:first]\nkey_a = val_a\n"
            "[printer:second]\nkey_b = val_b\n"
        )
        kv = parse_vendor_ini_section(ini, "first")
        self.assertIn("key_a", kv)
        self.assertNotIn("key_b", kv)

    def test_ignores_blank_lines_and_comments(self):
        ini = self._write_ini(
            "[printer:test]\n"
            "\n"
            "# this is a comment\n"
            "key = value\n"
        )
        kv = parse_vendor_ini_section(ini, "test")
        self.assertEqual(kv["key"], "value")
        self.assertEqual(len(kv), 1)

    def test_value_with_equals_sign(self):
        ini = self._write_ini(
            "[printer:test]\nformula = a=b=c\n"
        )
        kv = parse_vendor_ini_section(ini, "test")
        self.assertEqual(kv["formula"], "a=b=c")

    def test_section_substring_match(self):
        ini = self._write_ini(
            "[printer:*common_bioslicer_trident*]\nkey = found\n"
        )
        # Any substring of the section header should work
        for fragment in ["common_bioslicer_trident", "*common_bioslicer_trident*", "trident"]:
            with self.subTest(fragment=fragment):
                kv = parse_vendor_ini_section(ini, fragment)
                self.assertEqual(kv.get("key"), "found")


# ---------------------------------------------------------------------------
# bed_shape_bbox
# ---------------------------------------------------------------------------

class BedShapeBboxTests(unittest.TestCase):
    def test_bitrident_bed(self):
        min_x, min_y, max_x, max_y = bed_shape_bbox("77x79,177x79,177x134,77x134")
        self.assertAlmostEqual(min_x, 77.0)
        self.assertAlmostEqual(min_y, 79.0)
        self.assertAlmostEqual(max_x, 177.0)
        self.assertAlmostEqual(max_y, 134.0)

    def test_square_bed_origin(self):
        min_x, min_y, max_x, max_y = bed_shape_bbox("0x0,250x0,250x250,0x250")
        self.assertAlmostEqual(min_x, 0.0)
        self.assertAlmostEqual(min_y, 0.0)
        self.assertAlmostEqual(max_x, 250.0)
        self.assertAlmostEqual(max_y, 250.0)

    def test_single_point_degenerate(self):
        min_x, min_y, max_x, max_y = bed_shape_bbox("5x5,5x5,5x5,5x5")
        self.assertAlmostEqual(min_x, 5.0)
        self.assertAlmostEqual(max_x, 5.0)

    def test_negative_origin(self):
        min_x, min_y, max_x, max_y = bed_shape_bbox("-50x-50,50x-50,50x50,-50x50")
        self.assertAlmostEqual(min_x, -50.0)
        self.assertAlmostEqual(min_y, -50.0)
        self.assertAlmostEqual(max_x, 50.0)
        self.assertAlmostEqual(max_y, 50.0)


# ---------------------------------------------------------------------------
# write_slice_settings_ini output format
# ---------------------------------------------------------------------------

class WriteSliceSettingsIniTests(unittest.TestCase):
    def _write_and_read(self, **kwargs) -> dict[str, str]:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "slice.ini"
            write_slice_settings_ini(path, **kwargs)
            text = path.read_text(encoding="utf-8")
        kv: dict[str, str] = {}
        for line in text.splitlines():
            if "=" in line:
                k, _, v = line.partition("=")
                kv[k.strip()] = v.strip()
        return kv

    def test_single_extruder_csv_values(self):
        kv = self._write_and_read(extruder_count=1, layer_height=0.5, start_note="test")
        self.assertEqual(kv["nozzle_diameter"], "1.2")
        self.assertEqual(kv["filament_diameter"], "1.75")
        self.assertEqual(kv["temperature"], "205")
        self.assertEqual(kv["first_layer_temperature"], "210")

    def test_multi_extruder_csv_values(self):
        kv = self._write_and_read(extruder_count=3, layer_height=0.5, start_note="test")
        self.assertEqual(kv["nozzle_diameter"], "1.2,1.2,1.2")
        self.assertEqual(kv["filament_diameter"], "1.75,1.75,1.75")
        self.assertEqual(kv["temperature"], "205,205,205")
        self.assertEqual(kv["first_layer_temperature"], "210,210,210")

    def test_layer_height_reflected(self):
        kv = self._write_and_read(extruder_count=1, layer_height=1.5, start_note="test")
        self.assertEqual(kv["layer_height"], "1.5")
        self.assertEqual(kv["first_layer_height"], "1.5")

    def test_start_note_in_start_gcode(self):
        kv = self._write_and_read(extruder_count=1, layer_height=0.5, start_note="my print note")
        self.assertIn("my print note", kv["start_gcode"])

    def test_bed_shape_default(self):
        kv = self._write_and_read(extruder_count=1, layer_height=0.5, start_note="test")
        self.assertEqual(kv["bed_shape"], "0x0,250x0,250x250,0x250")

    def test_bed_shape_custom(self):
        kv = self._write_and_read(
            extruder_count=1, layer_height=0.5, start_note="test",
            bed_shape="77x79,177x79,177x134,77x134",
        )
        self.assertEqual(kv["bed_shape"], "77x79,177x79,177x134,77x134")

    def test_max_print_height_default(self):
        kv = self._write_and_read(extruder_count=1, layer_height=0.5, start_note="test")
        self.assertEqual(kv["max_print_height"], "250")

    def test_fixed_settings_present(self):
        kv = self._write_and_read(extruder_count=1, layer_height=0.5, start_note="test")
        self.assertEqual(kv["printer_technology"], "FFF")
        self.assertEqual(kv["gcode_flavor"], "klipper")
        self.assertEqual(kv["perimeters"], "1")
        self.assertEqual(kv["fill_density"], "0%")
        self.assertEqual(kv["travel_speed"], "200")
        self.assertEqual(kv["bed_temperature"], "55")


# ---------------------------------------------------------------------------
# write_sla_override_ini output format
# ---------------------------------------------------------------------------

class WriteSlaOverrideIniTests(unittest.TestCase):
    def _write_and_read(self, **kwargs) -> dict[str, str]:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "override.ini"
            write_sla_override_ini(path, **kwargs)
            text = path.read_text(encoding="utf-8")
        kv: dict[str, str] = {}
        for line in text.splitlines():
            if "=" in line:
                k, _, v = line.partition("=")
                kv[k.strip()] = v.strip()
        return kv

    def test_single_extruder_sla(self):
        kv = self._write_and_read(
            sla_flags=[1],
            video_names=["resin"],
            synth_flags=[1],
            embed_flags=[1],
            video_paths=[""],
            synth_width=320,
            synth_height=240,
            synth_fps=5,
            synth_lossless=True,
        )
        self.assertEqual(kv["sla_material_extruder"], "1")
        self.assertEqual(kv["sla_material_video_synthesize"], "1")
        self.assertEqual(kv["sla_material_video_names"], "resin")
        self.assertEqual(kv["sla_material_video_synth_width"], "320")
        self.assertEqual(kv["sla_material_video_synth_height"], "240")
        self.assertEqual(kv["sla_material_video_synth_fps"], "5")
        self.assertEqual(kv["sla_material_video_synth_lossless"], "1")

    def test_two_sla_extruders(self):
        kv = self._write_and_read(
            sla_flags=[1, 1],
            video_names=["ch1", "ch2"],
            synth_flags=[1, 1],
            embed_flags=[1, 1],
            video_paths=["", ""],
            synth_width=640,
            synth_height=360,
            synth_fps=6,
            synth_lossless=False,
        )
        self.assertEqual(kv["sla_material_extruder"], "1,1")
        self.assertEqual(kv["sla_material_video_synthesize"], "1,1")
        self.assertEqual(kv["sla_material_video_names"], "ch1;ch2")
        self.assertEqual(kv["sla_material_video_embed"], "1,1")
        self.assertEqual(kv["sla_material_video_synth_lossless"], "0")

    def test_mixed_fff_sla_extruders(self):
        """First extruder is FFF (flag=0), second is SLA (flag=1)."""
        kv = self._write_and_read(
            sla_flags=[0, 1],
            video_names=["", "resin"],
            synth_flags=[0, 1],
            embed_flags=[0, 1],
            video_paths=["", ""],
            synth_width=100,
            synth_height=100,
            synth_fps=1,
            synth_lossless=False,
        )
        self.assertEqual(kv["sla_material_extruder"], "0,1")
        self.assertEqual(kv["sla_material_video_synthesize"], "0,1")


# ---------------------------------------------------------------------------
# make_model_config_xml — extruder at object level
# ---------------------------------------------------------------------------

class MakeModelConfigXmlTests(unittest.TestCase):
    def _parse_kv(self, xml_bytes: bytes) -> list[dict]:
        import xml.etree.ElementTree as ET
        root = ET.fromstring(xml_bytes)
        results = []
        for meta in root.iter("metadata"):
            results.append(dict(meta.attrib))
        return results

    def test_object_level_extruder_present(self):
        obj = MeshObject(
            name="test_obj",
            extruder=3,
            vertices=[(0, 0, 0), (1, 0, 0), (0, 1, 0)],
            triangles=[(0, 1, 2)],
        )
        xml_bytes = make_model_config_xml([obj], "my_model")
        metas = self._parse_kv(xml_bytes)

        obj_extruder_metas = [
            m for m in metas
            if m.get("type") == "object" and m.get("key") == "extruder"
        ]
        self.assertEqual(len(obj_extruder_metas), 1)
        self.assertEqual(obj_extruder_metas[0]["value"], "3")

    def test_no_object_extruder_when_empty_objects(self):
        xml_bytes = make_model_config_xml([], "empty_model")
        metas = self._parse_kv(xml_bytes)
        obj_extruder_metas = [
            m for m in metas
            if m.get("type") == "object" and m.get("key") == "extruder"
        ]
        self.assertEqual(len(obj_extruder_metas), 0)


# ---------------------------------------------------------------------------
# write_3mf — structural sanity
# ---------------------------------------------------------------------------

class Write3mfTests(unittest.TestCase):
    def _make_obj(self, extruder: int = 1) -> MeshObject:
        verts, tris = make_box(0, 0, 0, 10, 10, 10)
        return MeshObject(name="box", extruder=extruder, vertices=verts, triangles=tris)

    def test_produces_valid_zip_with_model_entry(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "test.3mf"
            write_3mf(path, [self._make_obj()], model_name="test")
            self.assertTrue(path.exists())
            with zipfile.ZipFile(path) as zf:
                names = zf.namelist()
        self.assertIn("3D/3dmodel.model", names)

    def test_model_xml_contains_object_name(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "test.3mf"
            write_3mf(path, [self._make_obj()], model_name="my_test_model")
            with zipfile.ZipFile(path) as zf:
                xml = zf.read("3D/3dmodel.model").decode("utf-8")
        self.assertIn("my_test_model", xml)

    def test_config_xml_has_object_extruder(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "test.3mf"
            write_3mf(path, [self._make_obj(extruder=2)], model_name="test")
            with zipfile.ZipFile(path) as zf:
                xml = zf.read("Metadata/Slic3r_PE_model.config").decode("utf-8")
        # Object-level extruder metadata added by the staged change
        self.assertIn('key="extruder"', xml)
        self.assertIn('value="2"', xml)


if __name__ == "__main__":
    unittest.main()
