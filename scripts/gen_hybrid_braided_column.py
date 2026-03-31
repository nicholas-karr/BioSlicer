#!/usr/bin/env python3
"""Generate a tri-color braided column with an SLA light-pipe spine, then slice to G-code.

Envelope: 50 x 50 x 50 mm.
- E1/E2/E3: three braided bead paths winding around center.
- E4: SLA central spine with pulse nodes.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from typing import Dict, List, Tuple

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


def build_objects() -> List:
    layer_height = 1.0
    layers = 48
    center = 25.0
    radius = 12.0
    bead = 2.2

    grouped_vertices: Dict[int, List[Tuple[float, float, float]]] = {1: [], 2: [], 3: [], 4: []}
    grouped_triangles: Dict[int, List[Tuple[int, int, int]]] = {1: [], 2: [], 3: [], 4: []}

    for layer_idx in range(layers):
        zb = layer_idx * layer_height
        zt = zb + layer_height
        theta = (2.0 * math.pi * layer_idx) / 18.0

        for ribbon_idx, extruder in enumerate((1, 2, 3)):
            phase = theta + ribbon_idx * (2.0 * math.pi / 3.0)
            x = center + radius * math.cos(phase)
            y = center + radius * math.sin(phase)
            append_box(
                grouped_vertices,
                grouped_triangles,
                extruder,
                x - bead / 2.0,
                y - bead / 2.0,
                zb,
                x + bead / 2.0,
                y + bead / 2.0,
                zt,
            )

        # E4 resin spine with periodic bulbs that can be used as cue frames.
        append_box(grouped_vertices, grouped_triangles, 4, center - 1.0, center - 1.0, zb, center + 1.0, center + 1.0, zt)
        if layer_idx % 6 in (0, 1):
            append_box(grouped_vertices, grouped_triangles, 4, center - 2.0, center - 2.0, zb, center + 2.0, center + 2.0, zt)

    # Bottom and top capture rings for mechanical tie-in.
    for extruder in (1, 2, 3):
        append_box(grouped_vertices, grouped_triangles, extruder, 9.0, 9.0, 0.0, 41.0, 11.0, 1.0)
        append_box(grouped_vertices, grouped_triangles, extruder, 9.0, 39.0, 0.0, 41.0, 41.0, 1.0)
        append_box(grouped_vertices, grouped_triangles, extruder, 9.0, 9.0, 47.0, 41.0, 11.0, 48.0)
        append_box(grouped_vertices, grouped_triangles, extruder, 9.0, 39.0, 47.0, 41.0, 41.0, 48.0)

    return objects_from_grouped(grouped_vertices, grouped_triangles, prefix="braid")


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_bin = repo_root / "build" / "src" / "prusa-slicer"
    default_out_dir = repo_root / "build" / "generated"

    p = argparse.ArgumentParser(description="Generate braided column 3MF and G-code")
    p.add_argument("--prusa-slicer", type=Path, default=default_bin, help="Path to prusa-slicer binary")
    p.add_argument("--output-dir", type=Path, default=default_out_dir, help="Directory for generated files")
    p.add_argument("--name", default="hybrid_braided_column", help="Base name for generated files")
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

    objects = build_objects()
    write_3mf(path_3mf, objects, model_name=args.name)
    write_sla_override_ini(
        path_override_ini,
        sla_extruder=4,
        synth_width=synth_width,
        synth_height=synth_height,
        synth_fps=synth_fps,
        synth_lossless=synth_lossless,
        video_name="resin_spine_pulse",
    )

    run_slice(
        prusa_slicer_bin,
        path_3mf,
        path_gcode,
        path_override_ini,
        extruder_count=4,
        layer_height=1.0,
        start_note="Material map: T0/T1/T2 braided FFF strands, T3 SLA spine",
    )

    print(f"Wrote 3MF: {path_3mf}")
    print(f"Wrote G-code: {path_gcode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
