#!/usr/bin/env python3
"""Generate a voxel moire block with tri-color gradient planes and SLA marker lattice.

Envelope: 50 x 50 x 20 mm.
- E1/E2/E3: interleaved voxel fields creating orientation-dependent moire.
- E4: SLA marker lattice planes at selected Z slices.
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
    voxel = 2.0
    nx = 25
    ny = 25
    nz = 10  # 20 mm total

    grouped_vertices: Dict[int, List[Tuple[float, float, float]]] = {1: [], 2: [], 3: [], 4: []}
    grouped_triangles: Dict[int, List[Tuple[int, int, int]]] = {1: [], 2: [], 3: [], 4: []}

    for k in range(nz):
        z0 = k * voxel
        z1 = z0 + voxel

        for i in range(nx):
            for j in range(ny):
                # Keep a sparse occupancy so this stays a lattice, not a full solid brick.
                if ((i + j + k) % 3) != 0:
                    continue

                x0 = i * voxel
                y0 = j * voxel
                x1 = x0 + voxel * 0.9
                y1 = y0 + voxel * 0.9

                score = (2 * i + j + 3 * k) % 9
                if score < 3:
                    extruder = 1
                elif score < 6:
                    extruder = 2
                else:
                    extruder = 3

                append_box(grouped_vertices, grouped_triangles, extruder, x0, y0, z0, x1, y1, z1)

        # SLA reference lattice planes for imaging/alignment tests.
        if k in (2, 5, 8):
            for i in range(2, nx - 2, 4):
                append_box(
                    grouped_vertices,
                    grouped_triangles,
                    4,
                    i * voxel,
                    24.0,
                    z0,
                    i * voxel + 0.8,
                    26.0,
                    z1,
                )
            for j in range(2, ny - 2, 4):
                append_box(
                    grouped_vertices,
                    grouped_triangles,
                    4,
                    24.0,
                    j * voxel,
                    z0,
                    26.0,
                    j * voxel + 0.8,
                    z1,
                )

    return objects_from_grouped(grouped_vertices, grouped_triangles, prefix="moire")


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_bin = repo_root / "build" / "src" / "prusa-slicer"
    default_out_dir = repo_root / "build" / "generated"

    p = argparse.ArgumentParser(description="Generate voxel moire block 3MF and G-code")
    p.add_argument("--prusa-slicer", type=Path, default=default_bin, help="Path to prusa-slicer binary")
    p.add_argument("--output-dir", type=Path, default=default_out_dir, help="Directory for generated files")
    p.add_argument("--name", default="hybrid_voxel_moire_block", help="Base name for generated files")
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
        video_name="resin_moire_markers",
    )

    run_slice(
        prusa_slicer_bin,
        path_3mf,
        path_gcode,
        path_override_ini,
        extruder_count=4,
        layer_height=2.0,
        start_note="Material map: T0/T1/T2 FFF moire voxels, T3 SLA marker lattice",
    )

    print(f"Wrote 3MF: {path_3mf}")
    print(f"Wrote G-code: {path_gcode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
