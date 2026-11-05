#!/usr/bin/env python3
"""Extract embedded SLA videos from G-code and decode them into PNG frames."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from sla_video_runtime import extract_videos_from_gcode, safe_video_name


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    default_output = repo_root / "build" / "generated"

    parser = argparse.ArgumentParser(
        description=(
            "Extract embedded SLA MKV payloads from G-code and decode each video "
            "to a PNG frame sequence."
        )
    )
    parser.add_argument("gcode", type=Path, help="Path to source G-code file")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=default_output,
        help="Base output directory for extracted videos and frames",
    )
    parser.add_argument(
        "--frame-pattern",
        default="frame_%06d.png",
        help="ffmpeg output file pattern used inside each video frame directory",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Delete existing frame directories before decoding",
    )
    parser.add_argument(
        "--max-embedded-bytes",
        type=int,
        default=None,
        help=(
            "Reject an embedded SLA video whose declared size exceeds this many "
            "bytes, before any data is decoded. Unset by default (no limit); set "
            "this when decoding G-code from an untrusted source, since the "
            "declared size is otherwise attacker-controlled and bounds only how "
            "much is written to disk, not memory (decoding is already streamed)."
        ),
    )
    return parser.parse_args()


def decode_video_to_pngs(video_path: Path, frames_dir: Path, frame_pattern: str) -> None:
    if (
        Path(frame_pattern).name != frame_pattern
        or "%" not in frame_pattern
        or "\x00" in frame_pattern
    ):
        raise ValueError(
            "frame pattern must be a filename containing an ffmpeg sequence placeholder"
        )
    frames_dir.mkdir(parents=True, exist_ok=True)
    output_pattern = frames_dir / frame_pattern

    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(video_path),
        "-vsync",
        "0",
        str(output_pattern),
    ]

    subprocess.run(cmd, check=True)


def main() -> int:
    args = parse_args()

    if shutil.which("ffmpeg") is None:
        print("error: ffmpeg is required but was not found in PATH", file=sys.stderr)
        return 2

    gcode_path = args.gcode.resolve()
    if not gcode_path.exists():
        print(f"error: gcode not found: {gcode_path}", file=sys.stderr)
        return 2

    output_dir = args.output_dir.resolve()
    extracted_video_dir = output_dir / f"{gcode_path.stem}_videos"
    frames_root = output_dir / f"{gcode_path.stem}_frames"

    output_dir.mkdir(parents=True, exist_ok=True)
    extracted_video_dir.mkdir(parents=True, exist_ok=True)
    frames_root.mkdir(parents=True, exist_ok=True)

    extracted = extract_videos_from_gcode(
        str(gcode_path),
        str(extracted_video_dir),
        max_embedded_bytes=args.max_embedded_bytes,
    )
    if not extracted:
        print("No SLA videos found in G-code comments.")
        return 0

    print(f"Found {len(extracted)} SLA video payload(s) in {gcode_path}")

    for name, path_str in extracted.items():
        video_path = Path(path_str).expanduser().resolve()
        if not video_path.exists():
            raise RuntimeError(f"Video path does not exist for '{name}': {video_path}")

        frame_dir_name = safe_video_name(name)
        frames_dir = frames_root / frame_dir_name
        if args.clean and frames_dir.exists():
            shutil.rmtree(frames_dir)

        print(f"Decoding '{name}' from {video_path} -> {frames_dir}")
        decode_video_to_pngs(video_path, frames_dir, args.frame_pattern)

    print(f"Done. Frames written under: {frames_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
