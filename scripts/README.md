# Image and Video Display Scripts

Minimal TCP display stack for BioSlicer SLA video workflows.

## Quick Start

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp image-display-config.yaml.template image-display-config.yaml
python image-display.py
```

## MainsailOS (Pi 4) Defaults

The template defaults are tuned for MainsailOS:

- `video_cache_dir`: `/home/pi/printer_data/cache/bioslicer-sla-video-cache`
- `monitor_auto_detect`: `true`

Optional monitor probe:

```bash
python image-display.py --enum-monitors
```

## Systemd Service

Install and start the service:

```bash
sudo ./install-image-display-service.sh
```

Override user/path/display if needed:

```bash
sudo SERVICE_USER=pi SCRIPTS_DIR=/home/pi/BioSlicer/scripts DISPLAY_VALUE=:0 ./install-image-display-service.sh
```

Default unit name: `bioslicer-image-display.service`.

## Command Examples

```bash
python send-image.py localhost 5555 '{"type":"DISPLAY_IMAGE","path":"/data/frame.png"}'
python send-image.py localhost 5555 '{"type":"LOAD_VIDEOS_FROM_GCODE","gcode_path":"/data/job.gcode"}'
python send-image.py localhost 5555 '{"type":"SHOW_VIDEO_FRAME","name":"resin_a","frame":240}'
python send-image.py localhost 5555 '{"type":"CLEAR"}'
```

On localhost, `send-image.py` will try to start `bioslicer-image-display.service` if the first connection is refused.
Disable autostart with:

```bash
python send-image.py localhost 5555 --no-ensure-running '{"type":"CLEAR"}'
```

## Commands

- `DISPLAY_IMAGE`
- `CLEAR`
- `LOAD_VIDEO`
- `LOAD_VIDEOS_FROM_GCODE`
- `SHOW_VIDEO_FRAME`
- `LIST_VIDEOS`
- `UNLOAD_VIDEO`

## Typical SLA Flow

1. Configure SLA video fields in slicer custom G-code settings.
2. Export G-code.
3. Start the display server (or service).
4. Run macros from `scripts/macros/` to load videos and show frames during SLA steps.

## Hybrid Multi-Material Generators

Each script below generates both a `.3mf` and sliced `.gcode` in `build/generated/`.
They are designed for up to three FFF materials plus one SLA material, with geometry bounded to at most `50x50x50 mm`.

### 1) Tricolor Waffle Tower + SLA Stitch Nodes

```bash
python scripts/gen_hybrid_tricolor_waffle_tower.py
```

### 2) Braided Tri-Color Column + SLA Spine

```bash
python scripts/gen_hybrid_braided_column.py
```

### 3) Voxel Moire Block + SLA Marker Lattice

```bash
python scripts/gen_hybrid_voxel_moire_block.py
```

All scripts support:

```bash
--prusa-slicer <path>
--output-dir <dir>
--name <base_name>
--machine-settings-ini <ini>
--sla-synth-width <px>
--sla-synth-height <px>
--sla-synth-fps <fps>
--sla-synth-lossless
```

## Channel Setup Overrides (Scalable Logical Extruders)

For high-channel printers (for example 100 logical channels), generate one override INI per physical setup:

```bash
python scripts/gen_channel_setup_override.py \
	--output build/generated/setup_A_100ch.ini \
	--channels 100 \
	--default-nozzle 0.4 \
	--nozzle-map 1=0.4,2=0.6,3=0.25,4=0.8 \
	--sla-channels 2,17 \
	--embed-sla-video \
	--synthesize-sla-video
```

This writes vector options such as `nozzle_diameter`, `sla_material_extruder`, and `toolchange_gcode` so one printer profile can be reused across many hardware setups.
