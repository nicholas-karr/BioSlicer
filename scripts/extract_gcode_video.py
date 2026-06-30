#!/usr/bin/env python3
"""Extract an embedded SLA video from a BioSlicer gcode file."""

import argparse
import base64
import re
import sys
from pathlib import Path


def extract_video(gcode_path: Path, video_name: str | None, output_path: Path | None) -> None:
    in_video = False
    found_name = None
    expected_bytes = None
    chunks: list[str] = []

    with gcode_path.open("r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")

            if not in_video:
                m = re.match(
                    r"^;\s*bioslicer_sla_video[_ ]begin\s+name=(\S+)\s+extruder=\d+\s+bytes=(\d+)",
                    line,
                )
                if m:
                    name = m.group(1)
                    if video_name is None or name == video_name:
                        found_name = name
                        expected_bytes = int(m.group(2))
                        in_video = True
            else:
                if re.match(r"^;\s*bioslicer_sla_video\s+end\s+name=", line):
                    break
                m = re.match(r"^;\s*bioslicer_sla_video\s+(?:data\s+)?([A-Za-z0-9+/=]+)\s*$", line)
                if m:
                    chunks.append(m.group(1))

    if not chunks:
        desc = f"named {video_name!r} " if video_name else ""
        print(f"No embedded video {desc}found in gcode file.", file=sys.stderr)
        sys.exit(1)

    data = base64.b64decode("".join(chunks))

    if expected_bytes is not None and len(data) != expected_bytes:
        print(
            f"Warning: expected {expected_bytes} bytes, got {len(data)}",
            file=sys.stderr,
        )

    if output_path is None:
        output_path = gcode_path.with_name(f"{found_name or 'video'}.mkv")

    output_path.write_bytes(data)
    print(f"Wrote {len(data)} bytes to {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Extract an embedded SLA video from a BioSlicer gcode file."
    )
    parser.add_argument("gcode", type=Path, help="Input .gcode file")
    parser.add_argument("-n", "--name", default=None, help="Video name to extract (default: first)")
    parser.add_argument("-o", "--output", type=Path, default=None, help="Output file path")
    args = parser.parse_args()

    if not args.gcode.exists():
        print(f"File not found: {args.gcode}", file=sys.stderr)
        sys.exit(1)

    extract_video(args.gcode, args.name, args.output)


if __name__ == "__main__":
    main()
