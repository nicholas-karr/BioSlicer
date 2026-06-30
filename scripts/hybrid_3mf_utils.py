#!/usr/bin/env python3
"""Shared utilities for generating hybrid multi-material 3MF files and slicing to G-code."""

from __future__ import annotations

import subprocess
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple
from xml.etree import ElementTree as ET


@dataclass
class MeshObject:
    name: str
    extruder: int
    vertices: List[Tuple[float, float, float]]
    triangles: List[Tuple[int, int, int]]


def make_box(
    x0: float,
    y0: float,
    z0: float,
    x1: float,
    y1: float,
    z1: float,
) -> Tuple[List[Tuple[float, float, float]], List[Tuple[int, int, int]]]:
    v = [
        (x0, y0, z0),
        (x1, y0, z0),
        (x1, y1, z0),
        (x0, y1, z0),
        (x0, y0, z1),
        (x1, y0, z1),
        (x1, y1, z1),
        (x0, y1, z1),
    ]

    t = [
        (0, 2, 1),
        (0, 3, 2),
        (4, 5, 6),
        (4, 6, 7),
        (0, 1, 5),
        (0, 5, 4),
        (1, 2, 6),
        (1, 6, 5),
        (2, 3, 7),
        (2, 7, 6),
        (3, 0, 4),
        (3, 4, 7),
    ]
    return v, t


def append_box(
    grouped_vertices: Dict[int, List[Tuple[float, float, float]]],
    grouped_triangles: Dict[int, List[Tuple[int, int, int]]],
    extruder: int,
    x0: float,
    y0: float,
    z0: float,
    x1: float,
    y1: float,
    z1: float,
) -> None:
    verts, tris = make_box(x0, y0, z0, x1, y1, z1)
    v_offset = len(grouped_vertices[extruder])
    grouped_vertices[extruder].extend(verts)
    grouped_triangles[extruder].extend((a + v_offset, b + v_offset, c + v_offset) for a, b, c in tris)


def objects_from_grouped(
    grouped_vertices: Dict[int, List[Tuple[float, float, float]]],
    grouped_triangles: Dict[int, List[Tuple[int, int, int]]],
    prefix: str = "material",
) -> List[MeshObject]:
    objects: List[MeshObject] = []
    for extruder in sorted(grouped_vertices.keys()):
        if not grouped_triangles[extruder]:
            continue
        objects.append(
            MeshObject(
                name=f"{prefix}_E{extruder}",
                extruder=extruder,
                vertices=grouped_vertices[extruder],
                triangles=grouped_triangles[extruder],
            )
        )
    return objects


def _to_xml_bytes(root: ET.Element) -> bytes:
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def make_model_xml(objects: Sequence[MeshObject], model_name: str) -> bytes:
    model = ET.Element(
        "model",
        {
            "unit": "millimeter",
            "xml:lang": "en-US",
            "xmlns": "http://schemas.microsoft.com/3dmanufacturing/core/2015/02",
            "xmlns:slic3rpe": "http://schemas.prusa3d.com/slic3rpe/1.0",
        },
    )

    ET.SubElement(model, "metadata", {"name": "Application"}).text = "BioSlicer script"
    ET.SubElement(model, "metadata", {"name": "slic3rpe:Version3mf"}).text = "1"

    resources = ET.SubElement(model, "resources")
    combined_vertices: List[Tuple[float, float, float]] = []
    combined_triangles: List[Tuple[int, int, int]] = []
    for obj in objects:
        v_offset = len(combined_vertices)
        combined_vertices.extend(obj.vertices)
        combined_triangles.extend((a + v_offset, b + v_offset, c + v_offset) for a, b, c in obj.triangles)

    obj_el = ET.SubElement(resources, "object", {"id": "1", "type": "model", "name": model_name})
    mesh = ET.SubElement(obj_el, "mesh")
    vertices_el = ET.SubElement(mesh, "vertices")
    for x, y, z in combined_vertices:
        ET.SubElement(vertices_el, "vertex", {"x": f"{x:.6f}", "y": f"{y:.6f}", "z": f"{z:.6f}"})

    triangles_el = ET.SubElement(mesh, "triangles")
    for v1, v2, v3 in combined_triangles:
        ET.SubElement(triangles_el, "triangle", {"v1": str(v1), "v2": str(v2), "v3": str(v3)})

    build = ET.SubElement(model, "build")
    ET.SubElement(
        build,
        "item",
        {
            "objectid": "1",
            "transform": "1 0 0 0 1 0 0 0 1 0 0 0",
            "printable": "1",
        },
    )

    return _to_xml_bytes(model)


def make_model_config_xml(objects: Sequence[MeshObject], model_name: str) -> bytes:
    config = ET.Element("config")
    obj_el = ET.SubElement(config, "object", {"id": "1", "instances_count": "1"})
    ET.SubElement(obj_el, "metadata", {"type": "object", "key": "name", "value": model_name})
    # Store extruder at the object level so the UI's object-list extruder selector
    # can override it without being shadowed by a volume-level override.
    if objects:
        ET.SubElement(obj_el, "metadata", {"type": "object", "key": "extruder", "value": str(objects[0].extruder)})

    tri_cursor = 0
    for obj in objects:
        tri_count = len(obj.triangles)
        first_id = tri_cursor
        last_id = tri_cursor + tri_count - 1
        tri_cursor += tri_count

        vol_el = ET.SubElement(obj_el, "volume", {"firstid": str(first_id), "lastid": str(last_id)})
        ET.SubElement(vol_el, "metadata", {"type": "volume", "key": "name", "value": obj.name})
        ET.SubElement(vol_el, "metadata", {"type": "volume", "key": "volume_type", "value": "ModelPart"})
        ET.SubElement(vol_el, "metadata", {"type": "volume", "key": "matrix", "value": "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"})
        ET.SubElement(vol_el, "metadata", {"type": "volume", "key": "extruder", "value": str(obj.extruder)})
        ET.SubElement(
            vol_el,
            "mesh",
            {
                "edges_fixed": "0",
                "degenerate_facets": "0",
                "facets_removed": "0",
                "facets_reversed": "0",
                "backwards_edges": "0",
            },
        )

    return _to_xml_bytes(config)


def _content_types_xml() -> bytes:
    text = (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
        " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
        " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
        " <Default Extension=\"png\" ContentType=\"image/png\"/>\n"
        "</Types>"
    )
    return text.encode("utf-8")


def _relationships_xml() -> bytes:
    text = (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel-1\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
        "</Relationships>"
    )
    return text.encode("utf-8")


def write_3mf(path_3mf: Path, objects: Sequence[MeshObject], model_name: str) -> None:
    path_3mf.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path_3mf, mode="w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", _content_types_xml())
        zf.writestr("_rels/.rels", _relationships_xml())
        zf.writestr("3D/3dmodel.model", make_model_xml(objects, model_name=model_name))
        zf.writestr("Metadata/Slic3r_PE_model.config", make_model_config_xml(objects, model_name=model_name))


def load_simple_ini(path_ini: Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    for raw_line in path_ini.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def ini_int(values: Dict[str, str], key: str, fallback: int) -> int:
    value = values.get(key)
    if value is None:
        return fallback
    try:
        return int(value)
    except ValueError:
        return fallback


def ini_bool(values: Dict[str, str], key: str, fallback: bool) -> bool:
    value = values.get(key)
    if value is None:
        return fallback
    lowered = value.lower()
    if lowered in {"1", "true", "yes", "on"}:
        return True
    if lowered in {"0", "false", "no", "off"}:
        return False
    return fallback


def write_sla_override_ini(
    path_ini: Path,
    sla_flags: list,
    video_names: list,
    synth_flags: list,
    embed_flags: list,
    video_paths: list,
    synth_width: int,
    synth_height: int,
    synth_fps: int,
    synth_lossless: bool,
) -> None:
    def csv(vals) -> str:
        return ",".join(str(v) for v in vals)

    def semicsv(vals) -> str:
        return ";".join(str(v) for v in vals)

    path_ini.parent.mkdir(parents=True, exist_ok=True)
    content = "\n".join([
        f"sla_material_extruder = {csv(sla_flags)}",
        f"sla_material_video_synthesize = {csv(synth_flags)}",
        f"sla_material_video_names = {semicsv(video_names)}",
        f"sla_material_video_paths = {semicsv(video_paths)}",
        f"sla_material_video_embed = {csv(embed_flags)}",
        f"sla_material_video_synth_width = {synth_width}",
        f"sla_material_video_synth_height = {synth_height}",
        f"sla_material_video_synth_fps = {synth_fps}",
        f"sla_material_video_synth_lossless = {1 if synth_lossless else 0}",
        "toolchange_gcode = ; TOOLCHANGE next=[next_extruder]\\n; SLA name=[sla_video_name] path=[sla_video_path] embedded=[sla_video_embedded]\\n",
        "",
    ])
    path_ini.write_text(content, encoding="utf-8")


def write_slice_settings_ini(
    path_ini: Path,
    extruder_count: int,
    layer_height: float,
    start_note: str,
    bed_shape: str = "0x0,250x0,250x250,0x250",
    max_print_height: int = 250,
) -> None:
    csv = ",".join

    path_ini.parent.mkdir(parents=True, exist_ok=True)
    content = "\n".join([
        "printer_technology = FFF",
        f"bed_shape = {bed_shape}",
        f"max_print_height = {max_print_height}",
        "gcode_flavor = klipper",
        f"nozzle_diameter = {csv(['1.2'] * extruder_count)}",
        f"filament_diameter = {csv(['1.75'] * extruder_count)}",
        f"layer_height = {layer_height}",
        f"first_layer_height = {layer_height}",
        "perimeters = 1",
        "top_solid_layers = 0",
        "bottom_solid_layers = 0",
        "fill_density = 0%",
        "skirts = 0",
        "brim_width = 0",
        "travel_speed = 200",
        "perimeter_speed = 30",
        "infill_speed = 30",
        "solid_infill_speed = 30",
        f"temperature = {csv(['205'] * extruder_count)}",
        f"first_layer_temperature = {csv(['210'] * extruder_count)}",
        "bed_temperature = 55",
        "first_layer_bed_temperature = 55",
        f"start_gcode = ; {start_note}\\n",
        "end_gcode = M400\\n",
        "",
    ])
    path_ini.write_text(content, encoding="utf-8")


def run_slice(
    prusa_slicer_bin: Path,
    input_3mf: Path,
    output_gcode: Path,
    override_ini: Path,
    extruder_count: int,
    layer_height: float,
    start_note: str,
    bed_shape: str = "0x0,250x0,250x250,0x250",
) -> None:
    output_gcode.parent.mkdir(parents=True, exist_ok=True)

    slice_ini = output_gcode.with_suffix(".slice.ini")
    write_slice_settings_ini(slice_ini, extruder_count, layer_height, start_note, bed_shape)

    cmd = [
        str(prusa_slicer_bin),
        "--load", str(slice_ini),
        "--load", str(override_ini),
        str(input_3mf),
        "--export-gcode",
        "--output", str(output_gcode),
    ]

    print("Running:")
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    if not output_gcode.exists():
        raise RuntimeError(f"Slicing finished but output was not created: {output_gcode}")


# ---------------------------------------------------------------------------
# Vendor ini helpers
# ---------------------------------------------------------------------------

def parse_vendor_ini_section(ini_path: Path, section_substring: str) -> dict[str, str]:
    """Return key/value pairs from the first section whose header contains section_substring.

    Handles PrusaSlicer vendor bundle format where section names can contain
    colons and wildcards, e.g. ``[printer:*common_bioslicer_trident*]``.
    """
    result: dict[str, str] = {}
    in_section = False
    with ini_path.open(encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("["):
                in_section = section_substring in line
                continue
            if not in_section:
                continue
            if "=" not in line:
                continue
            key, _, val = line.partition("=")
            result[key.strip()] = val.strip()
    return result


def bed_shape_bbox(bed_shape_str: str) -> tuple[float, float, float, float]:
    """Parse ``77x79,177x79,177x134,77x134`` and return (min_x, min_y, max_x, max_y)."""
    xs, ys = [], []
    for point in bed_shape_str.split(","):
        x_str, _, y_str = point.strip().partition("x")
        xs.append(float(x_str))
        ys.append(float(y_str))
    return min(xs), min(ys), max(xs), max(ys)
