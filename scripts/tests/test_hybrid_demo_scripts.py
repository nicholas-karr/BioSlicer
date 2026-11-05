#!/usr/bin/env python3
"""Smoke tests for the gen_hybrid_*.py demo scripts using a fake slicer binary.

The fake slicer just records its --load configs and creates the --output file,
so these tests catch API drift between the scripts and hybrid_3mf_utils, and
slice settings the real slicer would reject.
"""

from __future__ import annotations

import configparser
import importlib
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))

DEMOS = [
    "gen_hybrid_3mf_and_slice",
    "gen_hybrid_braided_column",
    "gen_hybrid_tricolor_waffle_tower",
    "gen_hybrid_voxel_moire_block",
]

FAKE_SLICER = """#!/bin/sh
out=""
while [ $# -gt 0 ]; do
  if [ "$1" = "--output" ]; then out="$2"; fi
  shift
done
: > "$out"
"""


def read_flat_ini(path: Path) -> dict:
    cp = configparser.ConfigParser(interpolation=None)
    cp.read_string("[x]\n" + path.read_text())
    return dict(cp["x"])


class HybridDemoScriptTests(unittest.TestCase):
    def run_demo(self, name: str, tmp: Path) -> Path:
        slicer = tmp / "fake-slicer"
        slicer.write_text(FAKE_SLICER)
        slicer.chmod(slicer.stat().st_mode | stat.S_IXUSR)
        out = tmp / "out"
        mod = importlib.import_module(name)
        argv = sys.argv
        sys.argv = [name, "--prusa-slicer", str(slicer), "--output-dir", str(out)]
        try:
            self.assertEqual(mod.main(), 0)
        finally:
            sys.argv = argv
        return out

    def test_demos_run_and_write_valid_settings(self):
        for name in DEMOS:
            with self.subTest(demo=name), tempfile.TemporaryDirectory() as d:
                out = self.run_demo(name, Path(d))
                slice_ini = read_flat_ini(next(out.glob("*.slice.ini")))
                nozzle = min(float(v) for v in slice_ini["nozzle_diameter"].split(","))
                self.assertLessEqual(float(slice_ini["layer_height"]), nozzle)
                self.assertLessEqual(float(slice_ini["first_layer_height"]), nozzle)


if __name__ == "__main__":
    unittest.main()
