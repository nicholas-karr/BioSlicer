#!/usr/bin/env python3
"""Generate a hybrid-material 3MF example and slice it for a Voron-style bioprinter profile.

The model contains 3 layers, each 0.8 mm tall:
- layer 0: sparse parallel lines along X
- layer 1: sparse parallel lines along Y (rotated 90 degrees)
- layer 2: sparse parallel lines along X

Line ownership alternates by extruder/material:
- extruder 1: Generic PLA
- extruder 2: Generic SLA Resin (assumed hybrid workflow material)

Geometry is grouped into one shared volume per extruder/material (not one part per line).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Dict, List

from hybrid_3mf_utils import (
    append_box,
    ini_bool,
    ini_int,
    load_simple_ini,
    objects_from_grouped,
    run_slice,
    write_3mf,
    write_sla_override_ini,
)


def build_pattern_objects() -> List:
    bed_min = 20.0
    bed_max = 80.0
    line_pitch = 8.0
    line_width = 1.2
    layer_height = 0.8
    layers = 3

    grouped_vertices: Dict[int, List] = {1: [], 2: []}
    grouped_triangles: Dict[int, List] = {1: [], 2: []}

    for layer_idx in range(layers):
        z0 = layer_idx * layer_height
        z1 = z0 + layer_height
        middle_rotated = (layer_idx == 1)

        coord = bed_min
        line_idx = 0
        while coord + line_width <= bed_max + 1e-9:
            extruder = 1 if (line_idx % 2 == 0) else 2

            if middle_rotated:
                x0, x1 = coord, coord + line_width
                y0, y1 = bed_min, bed_max
            else:
                x0, x1 = bed_min, bed_max
                y0, y1 = coord, coord + line_width

            append_box(grouped_vertices, grouped_triangles, extruder, x0, y0, z0, x1, y1, z1)

            coord += line_pitch
            line_idx += 1

    return objects_from_grouped(grouped_vertices, grouped_triangles, prefix="voron_lines")


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_bin = repo_root / "build" / "src" / "prusa-slicer"
    default_out_dir = repo_root / "build" / "generated"

    p = argparse.ArgumentParser(description="Generate a hybrid-material 3MF and slice it.")
    p.add_argument("--prusa-slicer", type=Path, default=default_bin, help="Path to prusa-slicer binary")
    p.add_argument("--output-dir", type=Path, default=default_out_dir, help="Directory for generated files")
    p.add_argument("--name", default="voron_hybrid_3layer_lines", help="Base name for generated files")
    p.add_argument("--machine-settings-ini", type=Path, help="Printer/machine settings INI used for SLA synth defaults")
    p.add_argument("--sla-synth-width", type=int, default=None, help="Override synthesized video width in pixels")
    p.add_argument("--sla-synth-height", type=int, default=None, help="Override synthesized video height in pixels")
    p.add_argument("--sla-synth-fps", type=int, default=None, help="Override synthesized video frame rate")
    p.add_argument("--sla-synth-lossless", action="store_true", default=None, help="Override to lossless synthesized SLA encoding")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    prusa_slicer_bin = args.prusa_slicer.resolve()
    if not prusa_slicer_bin.exists():
        print(f"error: slicer binary not found: {prusa_slicer_bin}", file=sys.stderr)
        return 2

    out_dir = args.output_dir.resolve()
    path_3mf = out_dir / f"{args.name}.3mf"
    path_gcode = out_dir / f"{args.name}.gcode"
    path_override_ini = out_dir / f"{args.name}_sla_override.ini"

    machine_values = {}
    if args.machine_settings_ini is not None:
        machine_ini = args.machine_settings_ini.resolve()
        if not machine_ini.exists():
            print(f"error: machine settings ini not found: {machine_ini}", file=sys.stderr)
            return 2
        machine_values = load_simple_ini(machine_ini)

    synth_width = args.sla_synth_width if args.sla_synth_width is not None else ini_int(machine_values, "sla_material_video_synth_width", 3840)
    synth_height = args.sla_synth_height if args.sla_synth_height is not None else ini_int(machine_values, "sla_material_video_synth_height", 2160)
    synth_fps = args.sla_synth_fps if args.sla_synth_fps is not None else ini_int(machine_values, "sla_material_video_synth_fps", 30)
    synth_lossless = args.sla_synth_lossless if args.sla_synth_lossless is not None else ini_bool(machine_values, "sla_material_video_synth_lossless", False)

    objects = build_pattern_objects()
    write_3mf(path_3mf, objects, model_name=args.name)
    write_sla_override_ini(
        path_override_ini,
        sla_extruder=1,
        synth_width=synth_width,
        synth_height=synth_height,
        synth_fps=synth_fps,
        synth_lossless=synth_lossless,
        video_name="resin_basic",
    )

    run_slice(
        prusa_slicer_bin,
        path_3mf,
        path_gcode,
        path_override_ini,
        extruder_count=2,
        layer_height=0.8,
        start_note="Material map: T0 Generic PLA, T1 Generic SLA Resin (assumed hybrid profile)",
    )

    print(f"Wrote 3MF: {path_3mf}")
    print(f"Wrote G-code: {path_gcode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
