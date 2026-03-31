# BioSlicer Kalico Macros for SLA Video Materials

Use [bioslicer_sla_video_macros.cfg](bioslicer_sla_video_macros.cfg) to:

1. Load videos once at job start.
2. Reference videos by short name.
3. Show specific frames as the slicer requests SLA operations.

## MainsailOS Defaults

- `send-image.py` path: `/home/pi/BioSlicer/scripts/send-image.py`
- host/port: `127.0.0.1:5555`
- service name: `bioslicer-image-display.service`

If your install path or port differs, edit the `gcode_shell_command BIOSLICER_SLA_CMD` line.

## Toolchange Example

In Printer Settings > Custom G-code > Tool change G-code, you can use:

```
{% if sla_video_name != "" %}
BIOSLICER_SLA_TOOLCHANGE NAME=[sla_video_name] FRAME=[layer_num]
{% endif %}
```

SLA placeholders:

- `[sla_material_id]`
- `[sla_video_name]`
- `[sla_video_path]`
- `[sla_video_embedded]`

## Notes

- Embedded videos are loaded from comments via `LOAD_VIDEOS_FROM_GCODE`.
- External references use `[sla_video_path]` and can be pre-loaded with `BIOSLICER_SLA_LOAD_VIDEO`.
- Add your printer-specific movement, projector timing, and exposure control.
- With systemd, install/start the display server first: `scripts/install-image-display-service.sh`.
