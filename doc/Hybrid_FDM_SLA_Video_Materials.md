# Hybrid FDM + SLA Material Video Workflow

This document describes the BioSlicer workflow for using SLA materials in a regular FDM job,
and is the reference for the G-code BioSlicer emits for them.

## Scope

- Mixed jobs can include normal FDM tools and SLA-material tools.
- SLA material sequence data is represented as one H.265 MKV per SLA material.
- Slicer output supports:
  - embedded MKV payloads in base64 G-code comments,
  - or external MKV references.
- Printer-side consumption is controller-specific and outside this repository. The Klipper
  macros BioKalico ships for it are described in
  [bioslicer_macros.md](https://github.com/nicholas-karr/BioKalico/blob/main/biokalico_extras/bioslicer_macros.md).

## Conceptual Model

BioSlicer treats SLA materials as virtual extruders inside an otherwise normal FFF slicer. Each
SLA channel is assigned an extruder index (0-based) in the printer profile. Instead of
generating toolpaths, an SLA extruder contributes:

- a slice polygon at every layer where its geometry exists (used for video synthesis),
- a pre-encoded MKV video file (one frame per layer) embedded or referenced in the G-code,
- per-layer exposure commands (`BIOSLICER_SLA_EXPOSE` with the Klipper G-code flavor,
  `SLA_SHOW_FRAME` otherwise) that tell the projector which frame to expose.

The projector is not driven by continuous G-code motion; it is driven by a single command per
material per layer. All physical sequencing (resin bowl positioning, UV exposure timing) is the
printer's responsibility.

## New Printer Settings

These options are per extruder vectors. `sla_material_extruder`, the exposure timing
(`sla_material_exposure_time`, `sla_material_delay_before_exposure`,
`sla_material_delay_after_exposure`) and the layer transition
(`sla_material_transition_depth`, `sla_material_transition_time`) are edited on each
extruder's page; the others are edited as one list on the Custom G-code page:

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

In a profile file the per extruder vectors are indexed from 0, comma separated for boolean and
numeric values and semicolon separated for strings.

### `sla_material_extruder`

One boolean per extruder. `sla_material_extruder[i] = true` marks extruder `i` as an SLA
material tool. Everything else in the SLA pipeline is gated on this flag.

```ini
sla_material_extruder = 0,0,0,0,1,1,1,1
```

### `sla_material_video_synthesize`

`sla_material_video_synthesize[i] = true` generates an MKV natively during export, from the
slice polygons (see [Video Synthesis](#video-synthesis)). `false` uses an existing MKV
supplied through `sla_material_video_paths`.

When synthesis is enabled and `sla_material_video_paths[i]` is empty, the video is written to
a temporary file that is deleted after embedding. When the path is set, the synthesized file
is written there and kept.

### `sla_material_video_names`

`sla_material_video_names[i]` defines the short alias emitted in metadata. It is used:

- in the `bioslicer_sla_material_map` and `bioslicer_sla_video begin` header comments,
- in `BIOSLICER_MATERIAL_SWAP MATERIAL=<name>` (see [Material selection](#material-selection)),
- as the `sla_video_name` / `sla_material_id` placeholders in `toolchange_gcode`.

It is reduced to ASCII letters, digits, `_`, `-` and `.`, at most 128 characters, and is
prefixed with `sla_` when it would otherwise start with another character or consist of digits
only (the printer also registers every video under its 1-based position, which a digits-only
name would collide with). An empty name becomes `sla_t<extruder_id>`. If two SLA extruders end
up with the same name, the second gets `_t<extruder_id>` appended.

This name is not the material's identity on the printer. `SELECT_MATERIAL` uses the material
name of [Toolchange Placeholders](#toolchange-placeholders) (`[next_material]`), and the
exposure commands use a numeric position (see
[Video ordering and position numbers](#video-ordering-and-position-numbers)).

### `sla_material_video_paths`

`sla_material_video_paths[i]` points to the source/output MKV path. If synthesis is enabled
and embed mode is disabled, it must be set. With synthesis disabled, an extruder without a
path has no video.

### `sla_material_video_embed`

- `sla_material_video_embed[i] = true` emits base64 payload comments in G-code.
- `sla_material_video_embed[i] = false` emits an external reference comment (`path_b64`).

All BioTrident SLA channels embed. External references are for large files that the printer
loads from its own storage.

### Synthesis controls

| Key | Type | Default | Meaning |
|---|---|---|---|
| `sla_material_video_synth_width` | int | 1024 | Frame width in pixels |
| `sla_material_video_synth_height` | int | 1024 | Frame height in pixels |
| `sla_material_video_synth_fps` | int | 5 | Frame rate of the encoded MKV |
| `sla_material_video_synth_lossless` | bool | true | Encode with `lossless=1` |

Synthesis encoding uses software `libx265`; with lossless mode off it uses `-crf 18 -preset
medium`. The pixel size should match the projector's native resolution. The frame rate is
stored in the container but has no effect on print timing: the projector shows each frame for
as long as the printer waits, so a low value (such as 5) only keeps the container small.

### `channel_type` / `channel_label`

One string per extruder. `channel_type` is `fff`, `sla`, `liquid` or `droplet` in the shipped
profiles. It is the first part of a material's name (see
[Toolchange Placeholders](#toolchange-placeholders)), marks a [droplet](#droplets) channel,
and is what the compatible-printer conditions of filament profiles use to offer the right
materials for each channel. `channel_label` is the name shown for the channel.

### `sla_material_transition_depth` / `sla_material_transition_time`

Before each SLA material's own cure (every layer, not just on a material change), and after
that material has been put in place, the slicer moves the object `transition_depth`
millimetres away from that layer's cure height, holds for `transition_time` seconds, then
returns to cure height. Both default to `0`, which disables the move for that extruder (no
extra `G1 Z`/`G4` commands are emitted). See [Per-layer Commands](#per-layer-commands) for the
exact sequence.

```ini
sla_material_transition_depth = 0,0,0,0,-2.5,-2.5,-2.5,-2.5
sla_material_transition_time  = 0,0,0,0,1.5,1.5,1.5,1.5
```

## G-code File Structure

A BioSlicer G-code file has the following top-level layout:

```
; generated by BioSlicer <version> on <timestamp>
<thumbnail data>
<SLA material map comments>
<SLA video blocks (embedded) or ref comments (external)>
<print settings comments>
M73 P0 R<minutes>             (with remaining_times, see Progress reporting)
<start G-code>
SLA_LOAD_GCODE_VIDEOS         (right after the start G-code)
<temperature/preamble commands>
--- per-layer output ---
;LAYER_CHANGE
;Z:<print_z>
[FFF extrusion commands for this layer]
[SLA commands for this layer]
--- end of layer ---
<end G-code>
<filament usage summary>
; prusaslicer_config = begin
<full config dump>
; prusaslicer_config = end
```

### Progress reporting

A file carries three progress indicators:

- **Layer counter.** The start G-code sets
  `SET_PRINT_STATS_INFO TOTAL_LAYER=[total_layer_count]` and every layer sets `CURRENT_LAYER`.
  `total_layer_count` is the number of distinct layer heights in the print. Objects and copies
  cured side by side share their layers and are not counted separately.
- **`M73 P<percent> R<minutes>`.** Emitted when `remaining_times` is on, which the UTD HBL
  printer profiles set. The percentage is the share of the estimated print time that has
  elapsed, exposure and delay times included, so it advances with the exposures of an SLA-only
  print. It can lag by one layer. Mainsail shows it when its print progress calculation is set
  to "Slicer (M73)".
- **File position.** Mainsail's default "File position (relative)" calculation is the position
  between the first and the last command of the file. An embedded video is most of the file's
  bytes, so Moonraker takes the first `G`/`M` command after the last
  `; bioslicer_sla_video end` line as the start. Without that, a print would begin at the
  percentage of the file the videos take up.

## G-code Emission Protocol

When slicing ASCII G-code, BioSlicer emits one mapping line per SLA extruder that has a video
(embedded or external), before the video data:

```gcode
; bioslicer_sla_material_map extruder=1 name=resin_a embedded=1
```

| Field | Meaning |
|---|---|
| `extruder=<id>` | 0-based extruder index as known to the slicer |
| `name=<name>` | Sanitised name (from `sla_material_video_names`) |
| `embedded=<0\|1>` | `1` if the video data follows in a `begin` block; `0` if only a reference |

These lines act as a table of contents: a parser can read the whole material to video mapping
from them without reading any base64 data.

### Embedded payload format

```gcode
; bioslicer_sla_video begin name=resin_a extruder=1 bytes=123456 sha256=<digest>
; bioslicer_sla_video <base64 chunk>
; bioslicer_sla_video <base64 chunk>
; ...
; bioslicer_sla_video end name=resin_a
```

| Field | Meaning |
|---|---|
| `name=<name>` | Same sanitised name as the map line |
| `extruder=<id>` | 0-based extruder index |
| `bytes=<N>` | Exact byte count of the decoded MKV |
| `sha256=<digest>` | Lowercase hex SHA-256 of the raw MKV bytes |

All lines of a block share the `bioslicer_sla_video ` prefix. Data lines carry raw base64
(A-Z, a-z, 0-9, +, /, =): every line encodes 57 bytes as 76 characters, except the last one,
which is the only one that can be shorter and end in `=` padding. Any line between `begin` and
`end` that matches `^; bioslicer_sla_video [A-Za-z0-9+/=]+$` is a data chunk. Strip the prefix,
concatenate the chunks and base64-decode them to recover the MKV.

The `end` line repeats `name=<name>` so that a parser can verify block pairing without keeping
state from the `begin` line.

### External reference format

```gcode
; bioslicer_sla_video ref name=resin_a extruder=1 path_b64=L21udC92aWRlb3MvcmVzaW5fYS5ta3Y=
```

The path is the value of `sla_material_video_paths` for that extruder, base64 encoded so that
a path containing a line break cannot inject G-code lines. Older files carry it in plain text
as `path=<path>`; parsers accept both.

### Video ordering and position numbers

Videos are emitted in extruder-index order (lowest index first). The first video in the file
is position 1, the second is position 2, and so on, embedded and referenced videos alike. The
`NAME=<n>` of `SLA_SHOW_FRAME` and `BIOSLICER_SLA_EXPOSE` is this 1-based position, not the
`name=` of the header comments.

Example: two SLA extruders (indices 4 and 5) produce videos at positions 1 and 2. `NAME=1`
exposes the material of extruder 4, `NAME=2` that of extruder 5.

## Binary G-code Behavior

- SLA video metadata emission is ASCII-only.
- If `binary_gcode = true` and SLA video metadata would be emitted, export fails with `ExportError`.

## Loading the Videos

When the file carries at least one SLA video, embedded or referenced, BioSlicer emits
`SLA_LOAD_GCODE_VIDEOS` once, right after the start G-code. The printer reads the video
metadata of the file it is printing and registers each video under its 1-based position,
which is how the exposure commands address it. A print that exposes nothing (FFF only) does
not emit the command.

## How SLA Regions Are Sliced

### Perimeter generation is skipped

A region is SLA when its perimeter, infill and solid infill extruders are all the same SLA
extruder. A region that mixes FFF and SLA extruders, or two SLA extruders, across those roles
is rejected when slicing.

`Layer::make_perimeters()` skips SLA regions, leaving their `slices()` intact for video
synthesis:

- `LayerRegion::perimeters()`: always empty for SLA regions
- `LayerRegion::fills()`: always empty for SLA regions
- `LayerRegion::slices()`: populated and intact, used for video synthesis

The perimeters of an adjacent FFF region follow the union of the FFF and SLA geometry, so the
outer wall has no gap at the material boundary; infill stays inside the FFF geometry.

### Absence from tool ordering

`ToolOrdering::collect_extruders()` only adds an extruder to a layer's extruder list when it
has toolpaths, so SLA extruder IDs never appear in `layer_tools.extruders` and are invisible to
the standard extrusion pipeline.

### SLA-only prints

A print whose regions are all SLA has a tool ordering without extruders and is still valid.
Extruder 0 is used as a placeholder for the G-code writer; it is not a tool the print uses, so
`[initial_material]` is empty (see [Toolchange Placeholders](#toolchange-placeholders)).
Sequential printing (`complete_objects`) is rejected when an SLA region is present.

## Per-layer Commands

An SLA extruder has no toolpaths. On every layer it has geometry on, after the extrusions
of that layer, BioSlicer emits for each SLA extruder, in ascending extruder order:

```gcode
SLA_CLEAR                               ; projector dark
<material selection>                    ; only when the SLA material changes, see below
G1 Z<cure_z - transition_depth>         ; only if sla_material_transition_depth is not 0
G4 P<transition_time_ms>                ; only if sla_material_transition_time is not 0
G1 Z<cure_z>                            ; only if sla_material_transition_depth is not 0
BIOSLICER_SLA_EXPOSE EXTRUDER=<i> NAME=<n> FRAME=<f>   ; Klipper flavor
```

followed by one more `SLA_CLEAR` after the last material of the layer, so that the layer
change, the next layer's extrusions and a material change never happen under a lit
projector. In a single-material print `SLA_CLEAR` therefore appears twice per layer. Other
G-code flavors get `SLA_SHOW_FRAME NAME=<n> FRAME=<f>` surrounded by `G4` dwells
(`G4 P<delay_before_ms>` before it, `G4 P<exposure_ms>` and `G4 P<delay_after_ms>` after it)
instead of `BIOSLICER_SLA_EXPOSE`. `<n>` is the 1-based position of the material's
video in the file and `<f>` counts the layers the material has been exposed on: a layer
without geometry of that material has neither a frame in its video nor an exposure.

`BIOSLICER_SLA_EXPOSE` carries no times. `<i>` is the 0-based extruder index, and the printer
looks up the delay before exposure, the exposure time and the delay after it for that extruder
in `BIOSLICER_PROCESS_PARAMS`, which the file sets at its start and `BIOSLICER_OVERRIDE` can
change before a print.

The layer transition comes after the material selection, so that it happens in the resin that
is about to be cured and not in the one cured before.

`sla_material_transition_depth` is signed: a positive value moves to a lower Z coordinate,
a negative value to a higher one. Which of the two submerges the object depends on the
machine. A positive depth larger than the layer's height takes the move below Z = 0; the
slicer emits it as is (`G1 Z-…`), so the printer must allow it (`position_min` of
`[stepper_z]` in Klipper) and it is up to the user that the move is safe. Export fails only
if the move would go above `max_print_height`.

On an SLA-only layer the same commands follow the `;LAYER_CHANGE` marker and `;Z:` comment
with no extrusion before them. The slicer issues the layer's own Z move (`G1 Z<cure_z>`)
first. That move and the layer transition are plain G-code: the printer's macros are not
responsible for Z positioning, only for the projector and resin side of things.

The emission is implemented in `GCode.cpp` / `GCode.hpp`. For each layer the slicer walks the
regions of every object layer at that height, skips a region without slices on the layer (it
has no frame in its video either), and keeps the SLA extruder of every SLA region. The frame
counters are `m_sla_layer_counters` (keyed by 0-based extruder index, starting at 0, never
reset during a print), the video positions are `m_sla_video_index`, and the SLA extruder
exposed last is `m_last_sla_extruder`.

### Material selection

The material of an SLA extruder is selected the way a tool is, right before its first
exposure and whenever the SLA material changes. In a single-material print that is once per
job; with several SLA materials on a layer it recurs every layer, because the materials are
processed in the same ascending order each time. The selection is:

1. `toolchange_gcode`, if not empty, with the placeholders below resolved for the SLA extruder.
2. `BIOSLICER_MATERIAL_SWAP MATERIAL=<video name>`, unless `toolchange_omit_tool_command` is
   set and step 1 produced G-code. This is the SLA counterpart of `T<n>`.

A printer that selects materials by name therefore uses, for extruders and SLA materials alike:

```ini
toolchange_gcode = SELECT_MATERIAL MATERIAL=[next_material]
toolchange_omit_tool_command = 1
```

The BioTrident profile does, so an SLA material is selected by:

```gcode
SELECT_MATERIAL MATERIAL=SLA_GELMA_10_BIOSLICERTRIDENT
```

BioKalico's `SELECT_MATERIAL` finds the slot holding that material and, when it is not the
active one, runs the machine's own `BIOSLICER_MATERIAL_SWAP CHANNEL=<slot - 1>`. Those machine
macros take `CHANNEL`, not `MATERIAL`: a profile that leaves `toolchange_omit_tool_command`
off sends `BIOSLICER_MATERIAL_SWAP MATERIAL=<video name>`, which they treat as channel 0.

`toolchange_omit_tool_command` (off by default) stops the slicer from appending its own
command after a non-empty `toolchange_gcode`: the bare `T<n>` for an extruder,
`BIOSLICER_MATERIAL_SWAP` for an SLA material. The extruder number is then carried by a
`;TOOL_CHANGE:<n>` comment, which the G-code preview, statistics, cooling and pressure
equalizing read in place of `T<n>`. The printer does not know the slicer's tool numbers, so:

- temperatures are set without a tool index and only for the active tool: at the change after
  the first layer for the tool that is active then, for any other tool when it becomes active;
- two used extruders must not carry the same material, and ooze prevention is not available
  (both are rejected when slicing).

## Video Synthesis

### Slice polygon collection

`collect_sla_slice_expolys_per_layer()` aggregates geometry for a single SLA extruder across
all print objects and instances:

1. For each `PrintObject`, iterate `object->layers()`.
2. Take each `LayerRegion` classified as SLA whose SLA extruder matches the target extruder
   (1-indexed, converted to 0-indexed for comparison).
3. Collect every `surface.expolygon` from `layer_region->slices()`.
4. Translate by each `PrintInstance::shift` to account for object placement.
5. Key by `scale_(layer->print_z)` to group by Z height.
6. Return one `ExPolygons` vector per unique Z, sorted by Z ascending.

Each entry in the returned vector becomes one video frame. All instances of an object are
merged into one exposure polygon per layer, so the projector exposes them at the same time.

### Frame rasterisation

Each frame is rasterised to a PGM (portable grey map) file:

- Image size: `sla_material_video_synth_width` x `sla_material_video_synth_height` pixels
- Coordinate mapping: the full `bed_shape` bounding box maps to the full pixel grid (bed
  minimum to pixel (0,0), bed maximum to pixel (W-1, H-1))
- Exposed region (inside any ExPolygon): white (255)
- Unexposed region: black (0)

The PGM files are written to a temporary directory and deleted after encoding.

### Video encoding

ffmpeg is invoked with the sequence of PGM frames:

```
ffmpeg -y -hide_banner -loglevel error
       -framerate <fps> -f image2 -i frame_%06d.pgm
       -c:v libx265
       [-x265-params lossless=1 | -crf 18 -preset medium]
       -an -pix_fmt yuv420p
       <output.mkv>
```

The output is an MKV container with an H.265 (HEVC) video stream and no audio. Because the
input frames are black and white, lossless encoding produces very small files and is the
recommended setting.

### Frame count and layer correspondence

One frame is generated per unique Z value in the slice polygon data. If an SLA extruder has no
geometry on a layer, that layer does not contribute a frame. `FRAME=0` is the lowest layer the
material has geometry on, `FRAME=1` the next such layer, and so on. The per-layer commands
follow the same rule, so the frame counter and the video never drift apart, also when the
material only exists in part of the print's height.

## Toolchange Placeholders

`toolchange_gcode` is processed by the placeholder parser on every tool change of an extruder
and on every selection of an SLA material. The following placeholders are available in it, in
addition to the standard ones (`previous_extruder`, `next_extruder`, `layer_num`, `layer_z`,
...):

- `[next_material]`: the name of the next tool's material, `<CHANNEL TYPE>_<FILAMENT PRESET>`
  in upper case ASCII with everything else collapsed to `_`
  (for example `SLA_GELMA_10_BIOSLICERTRIDENT`); empty if it has no name. On BioKalico it must
  match the material's `params_slicer_token` in `printer/materials.cfg`.
- `[sla_material_id]`: the sanitised video name
- `[sla_video_name]`: same as `[sla_material_id]`
- `[sla_video_path]`: the value of `sla_material_video_paths` for the next tool
- `[sla_video_embedded]`: `1` if the video is embedded, `0` if it is referenced

The `sla_*` placeholders are empty/0 when the next tool is not an SLA material, so an empty
`[sla_material_id]` tells a tool change from an SLA material selection. When an SLA
material is selected, `previous_extruder` is the SLA extruder exposed before it, or -1.

Two more placeholders are available in every custom G-code: `[materials]`, the names of all
materials the print uses, comma separated, and `[initial_material]`, the material of the
first tool that extrudes. `[initial_material]` is empty for a print without extrusions (SLA
only), whose materials are all selected where they are first exposed.

`toolchange_gcode` does not run when only one extruder is in use, so the start G-code of the
BioTrident profile passes `[materials]` to `PRINT_START` and selects the first material
itself, skipping the command when there is none:

```
{if initial_material != ""}SELECT_MATERIAL MATERIAL=[initial_material]{endif}
```

Controller macros are deliberately not shipped by this repository.

## Indexing Rules

| Context | Index base | Source |
|---|---|---|
| `sla_material_extruder` config array access | 0-based | `get_at(ext0)` |
| `sla_material_video_names` config array access | 0-based | `get_at(ext0)` |
| `lr->region().extruder(frPerimeter)` return value | **1-based** | Convert: `ext0 = ext1 - 1` |
| `layer_tools.extruders` contents | 0-based | Direct use |
| `m_sla_layer_counters` key (frame counter map) | 0-based | `m_sla_layer_counters[ext0]` |
| `m_sla_video_index` key (position map) | 0-based | `m_sla_video_index[ext0]` |
| `m_sla_video_index` value (video position) | **1-based** | Used directly in `NAME=<n>` |
| `SLA_SHOW_FRAME NAME=<n>` | **1-based** | 1st video in file = NAME=1 |
| `SLA_SHOW_FRAME FRAME=<f>` | 0-based | 0 = first (bottom) layer |
| `previous_extruder` / `next_extruder` placeholders | 0-based | Direct use |

## BioTrident 250 Reference Configuration

```
Extruder count: 8
Extruders 0-3: FFF (sla_material_extruder = 0)
Extruders 4-7: SLA (sla_material_extruder = 1)

sla_material_extruder    = 0,0,0,0,1,1,1,1
sla_material_video_synthesize = 0,0,0,0,1,1,1,1
sla_material_video_embed = 0,0,0,0,1,1,1,1
channel_type             = fff;fff;fff;fff;sla;sla;sla;sla
channel_label            = FFF Nozzle 1 (0.4mm);FFF Nozzle 2 (0.4mm);
                           FFF Nozzle 3 (0.4mm);FFF Nozzle 4 (0.4mm);
                           SLA Bucket 1;SLA Bucket 2;SLA Bucket 3;SLA Bucket 4
```

In a hybrid print using FFF extruder 0 and SLA extruder 4:

- Extruder 4 is the first SLA extruder in the file, so it is `NAME=1`
- `SELECT_MATERIAL MATERIAL=<material of extruder 4>`, once
- `BIOSLICER_SLA_EXPOSE EXTRUDER=4 NAME=1 FRAME=<n>` on every layer extruder 4 has geometry on

In a 2-channel SLA print using extruders 4 and 5:

- Extruder 4 is `NAME=1`, extruder 5 is `NAME=2`
- Per layer: select the material of extruder 4, expose `NAME=1`, select the material of
  extruder 5, expose `NAME=2`

## Complete G-code Example

This example is a 2-channel SLA-only print of 3 layers. It uses BioTrident 250 extruders 4 and
5 (the first two SLA channels), holding Standard Photopolymer Resin and GelMA 10%, sliced with
the BioTrident profile (Klipper flavor, `toolchange_omit_tool_command = 1`) at 0.2 mm layers.
Both materials have `sla_material_transition_depth = -2.5` and
`sla_material_transition_time = 1.5`.

```gcode
; generated by BioSlicer ...

SET_GCODE_VARIABLE MACRO=BIOSLICER_PROCESS_PARAMS VARIABLE=sla_exposure VALUE={4:5,5:5,6:5,7:5}
SET_GCODE_VARIABLE MACRO=BIOSLICER_PROCESS_PARAMS VARIABLE=sla_delay_before VALUE={4:3,5:3,6:3,7:3}
SET_GCODE_VARIABLE MACRO=BIOSLICER_PROCESS_PARAMS VARIABLE=sla_delay_after VALUE={4:0,5:0,6:0,7:0}
; ... fff_first_layer_temp, fff_temp, bed_first_layer_temp, bed_temp ...
BIOSLICER_CHECK_PROCESS_OVERRIDES

; bioslicer_sla_material_map extruder=4 name=sla_t4 embedded=1
; bioslicer_sla_video begin name=sla_t4 extruder=4 bytes=739 sha256=<hex>
; bioslicer_sla_video <base64 data...>
; bioslicer_sla_video end name=sla_t4
; bioslicer_sla_material_map extruder=5 name=sla_t5 embedded=1
; bioslicer_sla_video begin name=sla_t5 extruder=5 bytes=739 sha256=<hex>
; bioslicer_sla_video <base64 data...>
; bioslicer_sla_video end name=sla_t5

SET_PRINT_STATS_INFO TOTAL_LAYER=3
PRINT_START MATERIALS=SLA_STANDARD_PHOTOPOLYMER_RESIN_BIOSLICERTRIDENT,SLA_GELMA_10_BIOSLICERTRIDENT
SLA_LOAD_GCODE_VIDEOS

;LAYER_CHANGE
;Z:0.2
G1 Z.2 F7800
SET_PRINT_STATS_INFO CURRENT_LAYER=1
SLA_CLEAR
SELECT_MATERIAL MATERIAL=SLA_STANDARD_PHOTOPOLYMER_RESIN_BIOSLICERTRIDENT
G1 Z2.7
G4 P1500
G1 Z.2
BIOSLICER_SLA_EXPOSE EXTRUDER=4 NAME=1 FRAME=0
SLA_CLEAR
SELECT_MATERIAL MATERIAL=SLA_GELMA_10_BIOSLICERTRIDENT
G1 Z2.7
G4 P1500
G1 Z.2
BIOSLICER_SLA_EXPOSE EXTRUDER=5 NAME=2 FRAME=0
SLA_CLEAR

;LAYER_CHANGE
;Z:0.4
G1 Z.4
SET_PRINT_STATS_INFO CURRENT_LAYER=2
SLA_CLEAR
SELECT_MATERIAL MATERIAL=SLA_STANDARD_PHOTOPOLYMER_RESIN_BIOSLICERTRIDENT
G1 Z2.9
G4 P1500
G1 Z.4
BIOSLICER_SLA_EXPOSE EXTRUDER=4 NAME=1 FRAME=1
SLA_CLEAR
SELECT_MATERIAL MATERIAL=SLA_GELMA_10_BIOSLICERTRIDENT
G1 Z2.9
G4 P1500
G1 Z.4
BIOSLICER_SLA_EXPOSE EXTRUDER=5 NAME=2 FRAME=1
SLA_CLEAR

;LAYER_CHANGE
;Z:0.6
G1 Z.6
SET_PRINT_STATS_INFO CURRENT_LAYER=3
SLA_CLEAR
SELECT_MATERIAL MATERIAL=SLA_STANDARD_PHOTOPOLYMER_RESIN_BIOSLICERTRIDENT
G1 Z3.1
G4 P1500
G1 Z.6
BIOSLICER_SLA_EXPOSE EXTRUDER=4 NAME=1 FRAME=2
SLA_CLEAR
SELECT_MATERIAL MATERIAL=SLA_GELMA_10_BIOSLICERTRIDENT
G1 Z3.1
G4 P1500
G1 Z.6
BIOSLICER_SLA_EXPOSE EXTRUDER=5 NAME=2 FRAME=2
SLA_CLEAR

PRINT_END
```

(Each `BIOSLICER_SLA_EXPOSE` is preceded in the file by two comment lines,
`;bioslicer_sla_params ...` and `;SLA_SHOW_FRAME`, that the slicer's own preview and time
estimate read; they are left out above.)

Observations:

- There is no `SELECT_MATERIAL` in the start G-code: no extruder is used, so
  `[initial_material]` is empty.
- `SLA_LOAD_GCODE_VIDEOS` appears exactly once, after the start G-code.
- `SLA_CLEAR` opens every material's sequence on every layer and closes every layer.
- `SELECT_MATERIAL` appears on every material change; with 2 materials that is twice per
  layer. With a single SLA material it appears once for the whole print.
- The `G1 Z<cure_z - depth>` / `G4 P<transition_time_ms>` / `G1 Z<cure_z>` triplet is the
  layer transition, made after the material is in place; it is omitted when
  `transition_depth` is `0` for that extruder.
- `NAME=1` and `NAME=2` are the 1st and 2nd video blocks in the file, not the video names
  `sla_t4`/`sla_t5`.
- `FRAME` starts at 0 and goes up by one per layer the material is exposed on.

## Offline extraction and testing

The repository intentionally does not provide a printer display service or
controller macros. To extract embedded payloads and decode them into PNG frames:

```bash
python scripts/extract_sla_frames_from_gcode.py path/to/job.gcode
```

`scripts/sla_video_runtime.py` is a dependency-free, streaming parser used by
that tool and by automated tests. Frame decoding is performed by the local
`ffmpeg` command.

## Droplets

A droplet experiment is a list of discrete points, each an (x, y, z) where a volume is dropped. Droplets
are dispensed by a droplet channel: an extruder whose `channel_type` is `droplet`. Like an SLA extruder it
has no toolpaths, and its material is selected by name with `toolchange_gcode`.

### Authoring

- **File > Generate Droplets** makes a grid (with a volume step per row and per column, for dose-response
  layouts), a line, or an array of SLA wells with their droplets. A well array is one object: a wall cured
  from an SLA material around every well and the droplets of every well, which move, copy and arrange with it.
- **File > Edit Droplets** opens the droplets gizmo. Click to add a droplet on the surface of the selected
  object (it then belongs to the object) or on the bed at the chosen height (it then belongs to the
  project). Click a droplet to select it and edit its numbers, right-click it to delete it.
- Droplets are saved in the 3MF file (`Metadata/BioSlicer_droplets.txt`) and take part in undo and redo.

A project with droplets and no object slices and exports.

### Settings

| Option | Where | Meaning |
|---|---|---|
| `filament_droplet_nl_per_us` | filament | volume per microsecond of valve pulse; the pulse of a droplet is its volume divided by it |
| `filament_droplet_frequency` | filament | rate at which several drops are fired at one point |
| `droplet_standoff` | printer, per extruder | height of the nozzle above the landing height |
| `droplet_settle_time` | printer, per extruder | wait after the move, before the pulse |
| `droplet_gcode` | printer | G-code of one droplet on flavors other than Klipper, with `[droplet_x]`, `[droplet_y]`, `[droplet_z]`, `[droplet_volume_nl]`, `[droplet_pulse_us]`, `[droplet_count]`, `[droplet_extruder]` |

Export fails for a droplet that needs a pulse of less than 100 us, or more than 10% duty at its frequency
(the limits of the printer's `pulse_pin` module), and for a droplet outside the bed or the printer's Z range.

### When a droplet is dispensed

The landing height `z` of a droplet decides the layer: a droplet is dispensed after the first layer whose
height is at least `z`. On that layer the order is the extrusions, the SLA exposures, the projector going
dark, then the droplets. A wall cured on a layer is therefore there to receive the droplets of that layer.
Droplets above the last layer are dispensed after it. Several droplet channels are dispensed one after the other.

The nozzle travels at one standoff above the layer. A set with *dip* goes down to the landing height plus the
standoff at each point instead, to dispense into a well, and comes back up before it moves over.

### G-code

For Klipper, each droplet is

```gcode
;bioslicer_droplet extruder=8 x=120.000 y=95.000 z=4.000 volume_nl=50 count=1 pulse_us=1000.0 frequency=2 settle_ms=200
BIOSLICER_DROPLET EXTRUDER=8 X=120.000 Y=95.000 Z=4.000 SAFE_Z=4.000 VOLUME_NL=50 COUNT=1
```

The command carries the volume, not the pulse time: the printer divides it by the volume per pulse in
`BIOSLICER_PROCESS_PARAMS` (`droplet_nl_per_us`, `droplet_frequency`, `droplet_settle`, keyed by extruder like
the SLA exposure times), which the file writes at its start and `BIOSLICER_OVERRIDE` can change before a print:

```gcode
BIOSLICER_OVERRIDE PARAM=droplet_nl_per_us EXTRUDER=8 VALUE=0.04
```

The comment line is read by the preview, which shows each droplet as a step of the moves slider and counts the
settle time and the drops in the time estimate. After the droplets the nozzle returns to the layer height, and
the tool of the next extrusions is selected again.

## Solid Editing: Booleans, Negative Volumes and Channels

BioSlicer can combine and cut solids exactly, so designs such as perfusable tissue blocks can be built
inside the slicer. All lengths are in millimetres. The operations use the CGAL mesh boolean backend and
work only on closed, non-self-intersecting solids. If an input is not valid, or the operation fails, a plain
message is shown and the scene is left unchanged. Every operation is one undo step.

- **Union, Subtract, Intersect.** Select two or more whole objects, or two or more solid parts of one
  object, then use *Edit > Solids* or the right-click menu. The first selected item is the base: Subtract
  removes the other items from it. The result keeps the name, extruder and settings of the base. For whole
  objects the result is a new object in bed coordinates and the originals are removed.
  Modifiers, negative volumes and support blockers cannot be combined.
- **Apply negative volumes.** Right-click an object (or use *Edit > Apply negative volumes*) to cut every
  negative volume out of every solid part and remove the negative volumes. Each part keeps its own
  material; a part that is cut away completely is removed, and if nothing would be left the operation is
  refused.
- **Subtract channel.** Select one object and choose *Subtract channel...*. Enter the path as one point per
  line (X Y Z in bed coordinates), the channel diameter, and optional inlet and outlet port lengths. A port
  prolongs the channel past its first or last point along the first or last segment, so a port that reaches
  past the surface opens the channel to the outside; a length of 0 leaves a closed end. The channel is one
  swept tube with mitred bends, and the polygon has the area of the circle, so its volume is exactly
  pi * (diameter / 2)^2 * path length, bends included. Bends sharper than 150 degrees, or segments too short
  for the diameter, are refused. The tube is cut in bed coordinates, so it stays round on scaled or rotated
  objects. Objects with several instances must be split into separate objects first, because the instances
  share one shape.
- **Result size.** After every operation a notification shows the volume in mm3 and ml and the bounding box
  (size and corners, mm) of the result in bed coordinates.
- **Boolean history.** A part made by Union, Subtract, Intersect, Apply negative volumes or Subtract channel
  remembers its chain (base, then each step). Choose *Boolean history...* to see it, switch steps off,
  change a step to another operation, and rebuild the part. The chain is stored in the part's own
  coordinates, so moving, rotating or scaling the part keeps it valid, and it is saved in the 3MF file and
  in undo snapshots. The shapes of the steps are stored copies: changing another part later does not
  change them. A step that fails on rebuild is named in the message and the part is left unchanged.

Painted supports, seams and multi-material painting on a changed part are removed, because they refer to
the old surface. The behaviour is covered by `tests/libslic3r/test_solid_edit.cpp`.

## Shells, primitives and dimensional accuracy

All dimensions in these tools are millimetres (angles in degrees) and the tests assert real numbers with the
tolerances below.

### Primitives

Right-click the empty plate, or an object, and choose **Add Shape** (or **Add part**, **Add negative volume**, **Add
modifier**), then **Torus**, **Ellipsoid**, **Capsule**, **Cone / Frustum**, **Tube** or **Wedge**. The same shapes are
in **Generate SLA Shape**. The dialog has one field per parameter with its unit; typed values are used exactly. It shows
the ideal volume and the largest volume loss caused by the segment count. Undo removes the added shape.

Vertices lie on the ideal surface, so a mesh is slightly smaller than the ideal solid. The bound used by the tests is
`primitive_volume_tolerance`: an inscribed n-gon keeps `sin(2 pi / n) / (2 pi / n)` of its circle's area, and the
rounded shapes add the same loss for the pole-to-pole rings. Examples: 180 segments lose about 0.02 %, 64 segments
about 0.16 %, 16 segments about 2.6 %. Wedge volume is exact. Cone, frustum and tube loss depends only on the
segment count.

To trade file size for accuracy, type the largest acceptable volume loss (%) in the dialog and press **Raise segments**.
Segment counts are raised to multiples of four, which also makes the bounding box exactly the typed size. The triangle
count grows with the product of the segment counts.

| Shape | Parameters (mm unless noted) | Ideal volume |
| --- | --- | --- |
| Torus | ring radius R, tube radius r, segments around ring, segments around tube | 2 pi^2 R r^2 |
| Ellipsoid | radius X, Y, Z, segments, rings | 4/3 pi a b c |
| Capsule | radius r, straight length L, segments, rings per end | pi r^2 L + 4/3 pi r^3 |
| Cone / Frustum | bottom radius, top radius, height, segments | pi h/3 (R^2 + R r + r^2) |
| Tube | outer radius, inner radius, height, segments | pi h (R^2 - r^2) |
| Wedge | length, width, high end height, low end height (0 gives a ramp) | length x width x (h1 + h2) / 2 |

### Offset, hollow shell and wall thickness

Select one object, right-click, **Shell and thickness tools**:

- **Offset surface**: grows (positive) or shrinks (negative) the surface by a distance in mm.
- **Make hollow shell**: wall thickness in mm, with no opening, one opening on top or openings on top and bottom (radius in
  mm). The outer surface is kept exactly, the cavity is a voxel offset of it. Independent of the SLA hollowing settings.
- **Measure wall thickness**: reports the thinnest wall in mm and its X, Y, Z position on the bed. It casts rays against the
  surface normal from four points on every triangle, so it needs a closed surface.

Each operation is one undo step. Offsets are voxel based; the voxel size is the larger of one eighth of the distance and
1/500 of the model size, so the surface error is a fraction of a voxel. Tests: a sphere shell stays within 1 % of
`4/3 pi (R^3 - r^3)` and its measured minimum wall is within 0.1 mm of the requested thickness.

### Dimensions report

**Shell and thickness tools, Dimensions report** lists, for the selected object as placed on the bed: size and bounding
box (mm), volume (mm3 and ml), surface area (mm2), the thinnest wall with its position, the triangle count and the
number of open edges (0 means a closed surface; otherwise volume and thickness may be wrong). **Save as CSV** writes one
line per instance with units in the column names, a `.` decimal separator and 6 decimals, so an experiment can record
exactly what was printed. Only model parts count; modifiers and negative volumes are ignored.

### Export precision

Tests export a part with typed dimensions placed at 150 mm and re-read it. Binary and ASCII STL, OBJ and 3MF (also with a
rotated, scaled instance) agree with the original within 0.001 mm (one micron) in size and vertex position.

### Typed size, position and rotation

The object info panel accepts exact size, position and rotation. In code, `apply_numeric_transform` sets the size along
the object axes, the rotation in degrees and the position of the bounding box centre. A 3mf save and load keeps
position to 0.0001 mm, size to 0.0001 mm and rotation to 0.001 degree.

## Vessel Tree Generator

*File > Generate Vessel Tree* makes a tree of tubes (blood vessels, perfusion channels) as one watertight object.
Select a tree it made and use the same menu entry to edit it. Every length is in millimetres. *How to make the tree*
picks one of three ways:

- *Grow by branching rules*: every vessel ends in a junction where 2 or 3 new vessels start, a set number of times.
- *Fill the chosen object*: vessels grow from the root towards points spread through the object chosen under *Keep
  the vessels inside this object*, until it is filled, and never leave it.
- *Use imported centreline points*: the tree follows a table of points and radii (*Import Points (CSV)*), for
  example the centrelines measured on a scan.

| Field | Meaning |
|-------|---------|
| Root vessel radius, length | Radius and length of the first tube. In fill mode the root runs straight for this length before it branches. |
| Number of branching levels | Levels of junctions. With 2 vessels per junction, *n* levels give 2^(n+1) - 1 tubes and 2^n ends (fewer if a limit below stops growth). |
| Vessels leaving each junction | 2 (bifurcation) or 3 (trifurcation). The first child is turned away in a random plane, the others are spread evenly around the parent axis. |
| Child length / parent length | Length of each child tube relative to its parent. |
| Radius at the end of a vessel / at its start | Tapering: each vessel narrows linearly to this fraction of its start radius (0.2 to 1). The children of a junction get their radii from the end radius of the parent. |
| Radius of child vessels | *Murray's law*: parent^3 = sum of the children^3, with the flow share of the first child setting how the radii divide; the other children share the rest equally (0.5 with 2 children gives two children of 0.794 times the parent radius). *Fixed ratio* gives every child the same fraction of the parent radius. |
| Branching angles | *Murray optimal* uses the angles that minimise pumping power: each child pulls along its axis with its radius squared, and the pulls balance the parent's. For 2 children cos(a) = (rp^4 + ra^4 - rb^4) / (2 rp^2 ra^2), which is 37.5 degrees per child for equal children; for 3 equal children it is 46.1 degrees. *Fixed angle* sets the angle between 2 children (10 to 170 degrees), shared in inverse proportion to their radii; with 3 children each leaves at half of it from the parent axis. |
| Random angle / length variation | Limits of a random change of each branching angle and length. The branching plane of every junction is also chosen at random. |
| Stop below this radius | Branches that would be thinner than this are not grown. |
| Growth step, Target points, Smallest distance from the object surface | Fill mode: vessels grow in steps of this length towards this many points spread through the object. No part of any vessel wall, end or junction comes closer to the object's surface than the distance given; vessels that would are cut off with everything they feed. Vessels that do not share a junction never touch. Radii follow Murray's law with equal flow to every end: a vessel feeding *k* of the *N* ends has the root radius times (k / N)^(1/3). |
| Random seed | The same settings and seed always give the same tree. |
| Wall thickness | 0 makes a solid tree with round ends. A larger value makes a hollow tree whose root and ends are open; the outer wall is at least this thick along the tubes and through the junctions (measured by ray casting in the tests). It must be smaller than the radius of the thinnest tube. Where two child lumens part, the wall between them (the carina) tapers to an edge, as in a real vessel. |
| Detail | Corners per cross section. The cross section is a regular polygon inscribed in the true circle, so the volume is low by 0.16 percent at 64 corners and 0.64 percent at 32. |
| Curved vessels | Fill and imported trees: *Smooth curves* sweeps a spline through the points, *Straight pieces* joins them with straight tubes that meet in exact mitre joints (the volume is then pi r^2 times the length of the polyline). A curve tighter than the vessel is wide is refused with a message naming the vessel. |
| Keep the vessels inside this object | Branching rules: tubes that would leave the chosen object are shortened to end one radius before its surface, or left out; they can still touch the surface where they run close to it at a shallow angle. Fill mode: the object to fill. The root start (X, Y, Z in bed coordinates) must be inside the object. |

A junction is the round end of the parent vessel when every child is at least 3 percent narrower than the parent, so
the surface does not bulge there; otherwise it is a ball 3 percent wider than the widest vessel that meets there.
Children start at the centre of the junction. Tests over random junctions (2 and 3 children, 10 to 170 degrees,
child radius 0.2 to 1 times the parent's, vessels as short as 1.05 parent radii, solid and hollow) check that the
mesh is watertight, manifold, free of zero-area triangles and self-intersections, that no vertex lies further than
5 percent of the local radius from a vessel axis, that the volume is within 1 percent of the exact union of the ideal
tubes, and that the outer wall is at least 98 percent of the thickness asked for.

*Export Points (CSV)* saves the centreline as a table for experiments: one row for every centreline point of every
vessel, with the columns `branch,parent,depth,point,x_mm,y_mm,z_mm,radius_mm`. `point` counts the points of a vessel
from 0 at its start, `parent` is -1 for the root, and the numbers read back as exactly the values the tree was built
from. Positions are relative to the root start (0, 0, 0) unless a bounding object is chosen, then they are bed
coordinates. *Import Points (CSV)* reads the same table (the header line is optional) in bed coordinates: vessels are
numbered 0, 1, 2 ... in order, every vessel names an earlier one as its parent and starts at its last point (up to
0.01 mm away is moved onto it), and a mistake is reported with its line number.

*Smallest feature your printer can make* sets the limit for the printability warnings that appear in red under the
preview. The tree is taken as the channel in a block (a hollow tree, as the channel its lumen). The warnings say
- how many channels are narrower than the limit, and the narrowest diameter (2 x the smallest radius along the vessel, so the cross section polygon is
  not taken into account);
- how many walls between two channels are thinner than the limit, the thinnest, and between which vessels (a vessel and
  its parent, or two siblings, are joined on purpose and not compared; a wall of zero or less means the channels touch);
- if the vessels are kept inside an object, how many channels come closer to its outside than the limit. This wall is
  measured sideways from the axis of each vessel, so a channel that opens through a face of the block is not reported
  there; the material in front of the closed end of a vessel is not measured. The value can be too large by up to
  0.3 percent of the distance from the axis to the surface.

The dialog only checks; it does not subtract the tree from a block.

The preview uses 12 corners and no wall; the object added to the bed uses the settings as given. Adding or editing a
tree is one undo step. The settings, and imported points, are stored in the 3mf project
(`Metadata/BioSlicer_vessel_trees.txt`), so a saved project can be edited later, and the tree can be regenerated
from them.

Surfaces are combined in exact arithmetic; slivers shorter than 1 micron are removed before the result is rounded to
the float vertices of the mesh, and the result is checked for zero-area triangles and self-intersections (the
pieces are turned slightly and combined again if it fails). The same code can sweep a single tube along a path of
points (straight or Catmull-Rom spline with a sample spacing in mm, a radius at every point, round or elliptical
section, round or flat ends, optional hollow lumen); it lives in `src/libslic3r/Vessel` and is covered by
`tests/libslic3r/test_vessel*.cpp`.
