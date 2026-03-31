# Hybrid FDM + SLA Material Video Workflow

This document describes the BioSlicer workflow for using SLA materials in a regular FDM job.

## Scope

- Mixed jobs can include normal FDM tools and SLA-material tools.
- SLA material sequence data is represented as one H.265 MKV per SLA material.
- Slicer output supports:
  - embedded MKV payloads in base64 G-code comments,
  - or external MKV references.
- Kalico macros can load videos by short name and display specific frames.

## New Printer Settings (Custom G-code page)

These options are per extruder vectors:

- `sla_material_extruder`
- `sla_material_video_synthesize`
- `sla_material_video_names`
- `sla_material_video_paths`
- `sla_material_video_embed`

These options are global synthesis controls:

- `sla_material_video_synth_width`
- `sla_material_video_synth_height`
- `sla_material_video_synth_fps`
- `sla_material_video_synth_lossless`

### Behavior

- `sla_material_extruder[i] = true` marks extruder `i` as an SLA material tool.
- `sla_material_video_synthesize[i] = true` generates an MKV natively during export.
- `sla_material_video_names[i]` defines the short alias used by macros.
- `sla_material_video_paths[i]` points to the source/output MKV path.
- `sla_material_video_embed[i] = true` emits base64 payload comments in G-code.
- `sla_material_video_embed[i] = false` emits an external reference comment (`path`).
- If synthesis is enabled and embed mode is disabled, `sla_material_video_paths[i]` must be set.
- Synthesis encoding uses software `libx265`; lossless mode is controlled by `sla_material_video_synth_lossless`.

## G-code Emission Protocol

When slicing ASCII G-code, BioSlicer emits one mapping line per configured SLA extruder:

```gcode
; bioslicer_sla_material_map extruder=1 name=resin_a embedded=1
```

### Embedded payload format

```gcode
; bioslicer_sla_video begin name=resin_a extruder=1 bytes=123456 sha256=<digest>
; bioslicer_sla_video <base64 chunk>
; bioslicer_sla_video <base64 chunk>
; ...
; bioslicer_sla_video end name=resin_a
```

### External reference format

```gcode
; bioslicer_sla_video ref name=resin_a extruder=1 path=/mnt/videos/resin_a.mkv
```

## Binary G-code Behavior

- SLA video metadata emission is ASCII-only.
- If `binary_gcode = true` and SLA video metadata would be emitted, export fails with `ExportError`.

## Toolchange Placeholders

The following placeholders are now available in `toolchange_gcode`:

- `[sla_material_id]`
- `[sla_video_name]`
- `[sla_video_path]`
- `[sla_video_embedded]`

These are empty/0 for non-SLA tool changes.

## Example Toolchange G-code

```gcode
{% if sla_video_name != "" %}
; Park and hand off to SLA motion path here
BIOSLICER_SLA_TOOLCHANGE NAME=[sla_video_name] FRAME=[layer_num]
{% endif %}
```

## Kalico Service + Macro Side

See:

- `scripts/image-display.py`
- `scripts/sla_video_runtime.py`
- `scripts/macros/bioslicer_sla_video_macros.cfg`

Recommended sequence:

1. Start display service before print.
2. At job start, call macro to `LOAD_VIDEOS_FROM_GCODE`.
3. On SLA steps/toolchanges, call `SHOW_VIDEO_FRAME` by alias and frame index.
4. Keep frame requests monotonic when possible to preserve decode state and reduce cost.

## Hardware Encode/Decode

- Decode is performed by ffmpeg with configured hardware acceleration (`ffmpeg_hwaccel`, optional decoder override).
