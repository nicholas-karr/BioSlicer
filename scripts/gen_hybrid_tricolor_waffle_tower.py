#!/usr/bin/env python3
"""Generate a 3-material waffle tower with SLA stitch nodes, then slice to G-code.

Envelope: 50 x 50 x 32 mm.
- E1: X-oriented ribs on alternating layers.
- E2: Y-oriented ribs on alternating layers.
- E3: perimeter lock-ring every layer.
- E4: sparse SLA stitch nodes at rib intersections every third layer.
"""

from __future__ import annotations

import argparse
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
    layer_height = 0.8
    layers = 40  # 32 mm total
    x0 = 0.0
    y0 = 0.0
    x1 = 50.0
    y1 = 50.0

    pitch = 6.0
    rib_width = 1.2
    ring_width = 1.4
    sla_node = 1.6

    grouped_vertices: Dict[int, List[Tuple[float, float, float]]] = {1: [], 2: [], 3: [], 4: []}
    grouped_triangles: Dict[int, List[Tuple[int, int, int]]] = {1: [], 2: [], 3: [], 4: []}

    for layer_idx in range(layers):
        zb = layer_idx * layer_height
        zt = zb + layer_height

        # E3 perimeter lock ring.
        append_box(grouped_vertices, grouped_triangles, 3, x0, y0, zb, x1, y0 + ring_width, zt)
        append_box(grouped_vertices, grouped_triangles, 3, x0, y1 - ring_width, zb, x1, y1, zt)
        append_box(grouped_vertices, grouped_triangles, 3, x0, y0 + ring_width, zb, x0 + ring_width, y1 - ring_width, zt)
        append_box(grouped_vertices, grouped_triangles, 3, x1 - ring_width, y0 + ring_width, zb, x1, y1 - ring_width, zt)

        if layer_idx % 2 == 0:
            c = y0 + 3.0
            while c + rib_width <= y1 - 3.0:
                append_box(grouped_vertices, grouped_triangles, 1, x0 + 3.0, c, zb, x1 - 3.0, c + rib_width, zt)
                c += pitch
        else:
            c = x0 + 3.0
            while c + rib_width <= x1 - 3.0:
                append_box(grouped_vertices, grouped_triangles, 2, c, y0 + 3.0, zb, c + rib_width, y1 - 3.0, zt)
                c += pitch

        # E4 SLA nodes for periodic through-thickness stitching.
        if layer_idx % 3 == 1:
            x = 8.0
            while x + sla_node <= 42.0:
                y = 8.0
                while y + sla_node <= 42.0:
                    append_box(grouped_vertices, grouped_triangles, 4, x, y, zb, x + sla_node, y + sla_node, zt)
                    y += 12.0
                x += 12.0

    return objects_from_grouped(grouped_vertices, grouped_triangles, prefix="waffle")


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_bin = repo_root / "build" / "src" / "prusa-slicer"
    default_out_dir = repo_root / "build" / "generated"

    p = argparse.ArgumentParser(description="Generate tricolor waffle tower 3MF and G-code")
    p.add_argument("--prusa-slicer", type=Path, default=default_bin, help="Path to prusa-slicer binary")
    p.add_argument("--output-dir", type=Path, default=default_out_dir, help="Directory for generated files")
    p.add_argument("--name", default="hybrid_tricolor_waffle_tower", help="Base name for generated files")
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
        video_name="resin_waffle_nodes",
    )

    run_slice(
        prusa_slicer_bin,
        path_3mf,
        path_gcode,
        path_override_ini,
        extruder_count=4,
        layer_height=0.8,
        start_note="Material map: T0/T1/T2 FFF ribs, T3 SLA stitch nodes",
    )

    print(f"Wrote 3MF: {path_3mf}")
    print(f"Wrote G-code: {path_gcode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
