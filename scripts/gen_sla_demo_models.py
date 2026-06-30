#!/usr/bin/env python3
"""Generate basic 3D demo shapes for BioSlicer SLA video demonstration.

Shapes: cube, cylinder, cone, sphere, ring, pyramid
Each model contains an FFF base plate (extruder 1) and an SLA main body (extruder 2).

Usage:
  python3 gen_sla_demo_models.py [--shape cube] [--size 20] [--output-dir ./generated]

Output:
  Prints "3MF: <path>" on success so callers can parse the generated file path.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from typing import Dict, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from hybrid_3mf_utils import append_box, objects_from_grouped, write_3mf

SHAPES: Tuple[str, ...] = ("cube", "cylinder", "cone", "sphere", "ring", "pyramid")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_Groups = Tuple[Dict[int, list], Dict[int, list]]


def _groups() -> _Groups:
    return {1: [], 2: []}, {1: [], 2: []}


def _add_base(gv: Dict, gt: Dict, size: float, base_h: float) -> None:
    """FFF base plate that sits beneath the SLA body."""
    append_box(gv, gt, 1, 0.0, 0.0, 0.0, size, size, base_h)


# ---------------------------------------------------------------------------
# Shape builders
# ---------------------------------------------------------------------------

def build_cube(size: float, base_h: float, **_kw) -> list:
    """Simple solid cube — one SLA box on an FFF base."""
    gv, gt = _groups()
    _add_base(gv, gt, size, base_h)
    append_box(gv, gt, 2, 0.0, 0.0, base_h, size, size, base_h + size)
    return objects_from_grouped(gv, gt, "cube")


def build_cylinder(size: float, base_h: float, voxel: float, **_kw) -> list:
    """Cylinder inscribed in the bounding box, voxelised with axis-aligned columns."""
    gv, gt = _groups()
    _add_base(gv, gt, size, base_h)
    cx = cy = size / 2.0
    r = size / 2.0
    x = 0.0
    while x + voxel <= size + 1e-9:
        y = 0.0
        while y + voxel <= size + 1e-9:
            mx, my = x + voxel / 2.0, y + voxel / 2.0
            if (mx - cx) ** 2 + (my - cy) ** 2 <= r ** 2:
                append_box(gv, gt, 2, x, y, base_h, x + voxel, y + voxel, base_h + size)
            y += voxel
        x += voxel
    return objects_from_grouped(gv, gt, "cylinder")


def build_cone(size: float, base_h: float, voxel: float, layer_h: float, **_kw) -> list:
    """Cone with full radius at the base, tapering to a point at the top."""
    gv, gt = _groups()
    _add_base(gv, gt, size, base_h)
    cx = cy = size / 2.0
    r_base = size / 2.0
    layers = max(1, round(size / layer_h))
    for li in range(layers):
        z0 = base_h + li * layer_h
        z1 = z0 + layer_h
        r = r_base * (1.0 - (li + 0.5) / layers)
        x = cx - r_base
        while x + voxel <= cx + r_base + 1e-9:
            y = cy - r_base
            while y + voxel <= cy + r_base + 1e-9:
                mx, my = x + voxel / 2.0, y + voxel / 2.0
                if (mx - cx) ** 2 + (my - cy) ** 2 <= r ** 2:
                    append_box(gv, gt, 2, x, y, z0, x + voxel, y + voxel, z1)
                y += voxel
            x += voxel
    return objects_from_grouped(gv, gt, "cone")


def build_sphere(size: float, base_h: float, voxel: float, layer_h: float, **_kw) -> list:
    """Full sphere (diameter = size), voxelised per layer."""
    gv, gt = _groups()
    _add_base(gv, gt, size, base_h)
    cx = cy = size / 2.0
    r = size / 2.0
    layers = max(1, round(2 * r / layer_h))
    for li in range(layers):
        z0 = base_h + li * layer_h
        z1 = z0 + layer_h
        h = (li + 0.5) * layer_h
        dh = h - r
        if abs(dh) >= r:
            continue
        local_r = math.sqrt(max(0.0, r ** 2 - dh ** 2))
        x = cx - r
        while x + voxel <= cx + r + 1e-9:
            y = cy - r
            while y + voxel <= cy + r + 1e-9:
                mx, my = x + voxel / 2.0, y + voxel / 2.0
                if (mx - cx) ** 2 + (my - cy) ** 2 <= local_r ** 2:
                    append_box(gv, gt, 2, x, y, z0, x + voxel, y + voxel, z1)
                y += voxel
            x += voxel
    return objects_from_grouped(gv, gt, "sphere")


def build_ring(size: float, base_h: float, voxel: float, inner_fraction: float = 0.45, **_kw) -> list:
    """Hollow cylinder (ring cross-section).  Inner radius = outer * inner_fraction."""
    gv, gt = _groups()
    _add_base(gv, gt, size, base_h)
    cx = cy = size / 2.0
    r_outer = size / 2.0
    r_inner = r_outer * inner_fraction
    x = 0.0
    while x + voxel <= size + 1e-9:
        y = 0.0
        while y + voxel <= size + 1e-9:
            mx, my = x + voxel / 2.0, y + voxel / 2.0
            d2 = (mx - cx) ** 2 + (my - cy) ** 2
            if r_inner ** 2 <= d2 <= r_outer ** 2:
                append_box(gv, gt, 2, x, y, base_h, x + voxel, y + voxel, base_h + size)
            y += voxel
        x += voxel
    return objects_from_grouped(gv, gt, "ring")


def build_pyramid(size: float, base_h: float, voxel: float, layer_h: float, **_kw) -> list:
    """Square pyramid with full base, tapering to a point at the top."""
    gv, gt = _groups()
    _add_base(gv, gt, size, base_h)
    cx = cy = size / 2.0
    layers = max(1, round(size / layer_h))
    for li in range(layers):
        z0 = base_h + li * layer_h
        z1 = z0 + layer_h
        half = (size / 2.0) * (1.0 - (li + 0.5) / layers)
        x = cx - half
        while x + voxel <= cx + half + 1e-9:
            y = cy - half
            while y + voxel <= cy + half + 1e-9:
                append_box(gv, gt, 2, x, y, z0, x + voxel, y + voxel, z1)
                y += voxel
            x += voxel
    return objects_from_grouped(gv, gt, "pyramid")


_BUILDERS = {
    "cube":     build_cube,
    "cylinder": build_cylinder,
    "cone":     build_cone,
    "sphere":   build_sphere,
    "ring":     build_ring,
    "pyramid":  build_pyramid,
}

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    default_out = Path(__file__).resolve().parent.parent / "generated"
    p = argparse.ArgumentParser(
        description="Generate a BioSlicer SLA demo 3MF.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--shape",       choices=SHAPES, default="cube",  help="Shape to generate")
    p.add_argument("--size",        type=float, default=20.0, metavar="MM",  help="Bounding box size in mm")
    p.add_argument("--base-height", type=float, default=1.0,  metavar="MM",  help="FFF base plate height in mm")
    p.add_argument("--voxel-size",  type=float, default=1.5,  metavar="MM",  help="Grid resolution for curved shapes in mm")
    p.add_argument("--layer-height",type=float, default=0.5,  metavar="MM",  help="Layer height used for per-layer shapes in mm")
    p.add_argument("--output-dir",  type=Path,  default=default_out, metavar="DIR", help="Directory for generated files")
    p.add_argument("--name",        default="",  help="Base file name (default: <shape>_sla_demo)")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    name    = args.name or f"{args.shape}_sla_demo"
    out_dir = args.output_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    path_3mf = out_dir / f"{name}.3mf"

    objects = _BUILDERS[args.shape](
        size=args.size,
        base_h=args.base_height,
        voxel=args.voxel_size,
        layer_h=args.layer_height,
    )

    if not any(obj.triangles for obj in objects if obj.extruder != 1):
        print(f"error: shape '{args.shape}' produced no geometry "
              f"(try a larger --size or smaller --voxel-size)", file=sys.stderr)
        return 1

    write_3mf(path_3mf, objects, model_name=name)
    print(f"3MF: {path_3mf}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
