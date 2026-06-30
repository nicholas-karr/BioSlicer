#!/usr/bin/env python3
"""Utilities for loading and streaming SLA video payloads for Kalico macros."""

from __future__ import annotations

import base64
import hashlib
import json
import logging
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional, Tuple


logger = logging.getLogger(__name__)


def _decode_b64_ascii(value: str) -> str:
    data = base64.b64decode(value.encode("ascii"), validate=True)
    return data.decode("utf-8")


def _safe_name_for_file(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]", "_", name)


def extract_videos_from_gcode(gcode_path: str, output_dir: str) -> Dict[str, str]:
    """Extract embedded videos and references from G-code comments.

    Supported comment protocol:
    - ; bioslicer_sla_video begin name=<name> ...
    - ; bioslicer_sla_video <base64>
    - ; bioslicer_sla_video end name=<name>
    - ; bioslicer_sla_video ref name=<name> path=<utf8 path>
    """
    begin_re = re.compile(
        r"^;\s*bioslicer_sla_video[_ ]begin\s+name=(\S+)\s+extruder=(\d+)\s+bytes=(\d+)(?:\s+sha256=([0-9a-fA-F]{64}))?\s*$"
    )
    data_re = re.compile(r"^;\s*bioslicer_sla_video\s+(?:data\s+)?([A-Za-z0-9+/=]+)\s*$")
    # Old binary format: bare "; <base64>" data lines with no keyword prefix
    bare_data_re = re.compile(r"^;\s*([A-Za-z0-9+/=]{4,})\s*$")
    end_re = re.compile(r"^;\s*bioslicer_sla_video\s+end\s+name=(\S+)\s*$")
    ref_path_re = re.compile(r"^;\s*bioslicer_sla_video\s+ref\s+name=(\S+)\s+extruder=(\d+)\s+path=(.+)$")
    ref_path_b64_re = re.compile(
        r"^;\s*bioslicer_sla_video\s+ref\s+name=(\S+)\s+extruder=(\d+)\s+path_b64=(\S+)\s*$"
    )

    extracted: Dict[str, str] = {}
    active_name: Optional[str] = None
    active_expected_bytes = 0
    active_expected_sha256 = ""
    active_chunks: list[str] = []

    output_base = Path(output_dir)
    output_base.mkdir(parents=True, exist_ok=True)

    with open(gcode_path, "r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\n")

            ref_match = ref_path_re.match(line)
            if ref_match:
                name = ref_match.group(1)
                extracted[name] = ref_match.group(3).strip()
                continue

            ref_b64_match = ref_path_b64_re.match(line)
            if ref_b64_match:
                name = ref_b64_match.group(1)
                path_b64 = ref_b64_match.group(3)
                extracted[name] = _decode_b64_ascii(path_b64)
                continue

            begin_match = begin_re.match(line)
            if begin_match:
                if active_name is not None:
                    raise RuntimeError(
                        f"Found nested embedded video block while parsing {gcode_path}."
                    )
                active_name = begin_match.group(1)
                active_expected_bytes = int(begin_match.group(3))
                active_expected_sha256 = (begin_match.group(4) or "").lower()
                active_chunks = []
                continue

            if active_name is None:
                continue

            data_match = data_re.match(line) or bare_data_re.match(line)
            if data_match:
                active_chunks.append(data_match.group(1))
                continue

            end_match = end_re.match(line)
            if end_match:
                end_name = end_match.group(1)
                if end_name != active_name:
                    raise RuntimeError(
                        f"Mismatched embedded video terminator: expected {active_name}, got {end_name}."
                    )

                payload = base64.b64decode("".join(active_chunks).encode("ascii"), validate=True)
                if len(payload) != active_expected_bytes:
                    raise RuntimeError(
                        f"Embedded payload length mismatch for {active_name}: "
                        f"expected {active_expected_bytes}, got {len(payload)}."
                    )

                if active_expected_sha256:
                    digest = hashlib.sha256(payload).hexdigest()
                    if digest != active_expected_sha256:
                        raise RuntimeError(
                            f"Embedded payload digest mismatch for {active_name}: "
                            f"expected {active_expected_sha256}, got {digest}."
                        )

                output_path = output_base / f"{_safe_name_for_file(active_name)}.mkv"
                with open(output_path, "wb") as video_handle:
                    video_handle.write(payload)

                extracted[active_name] = str(output_path)
                active_name = None
                active_expected_bytes = 0
                active_expected_sha256 = ""
                active_chunks = []

    if active_name is not None:
        raise RuntimeError(
            f"Unterminated embedded video block for {active_name} in {gcode_path}."
        )

    return extracted


@dataclass
class VideoMetadata:
    name: str
    path: str
    width: int
    height: int
    fps: float
    frame_count: int
    codec: str


class FFmpegVideoStream:
    """Persistent ffmpeg raw RGBA stream with frame-index state."""

    def __init__(
        self,
        name: str,
        path: str,
        hwaccel: str = "auto",
        hw_decoder: Optional[str] = None,
    ) -> None:
        self.name = name
        self.path = os.path.abspath(os.path.expanduser(path))
        self.hwaccel = hwaccel
        self.hw_decoder = hw_decoder
        self.process: Optional[subprocess.Popen[bytes]] = None
        self.current_frame = 0

        self.metadata = self._probe_video(self.path)
        self.frame_size_bytes = self.metadata.width * self.metadata.height * 4

    def _probe_video(self, path: str) -> VideoMetadata:
        cmd = [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,avg_frame_rate,nb_frames,codec_name:format=duration",
            "-of",
            "json",
            path,
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"ffprobe failed for {path}: {proc.stderr.strip()}")

        payload = json.loads(proc.stdout)
        streams = payload.get("streams", [])
        if not streams:
            raise RuntimeError(f"No video stream found in {path}")

        stream = streams[0]
        width = int(stream.get("width", 0))
        height = int(stream.get("height", 0))
        if width <= 0 or height <= 0:
            raise RuntimeError(f"Invalid video dimensions in {path}: {width}x{height}")

        fps = 0.0
        avg_frame_rate = stream.get("avg_frame_rate", "0/1")
        try:
            num_str, den_str = avg_frame_rate.split("/", 1)
            num = float(num_str)
            den = float(den_str)
            if den > 0:
                fps = num / den
        except Exception:
            fps = 0.0

        frame_count = 0
        nb_frames = stream.get("nb_frames")
        if nb_frames is not None and str(nb_frames).isdigit():
            frame_count = int(nb_frames)
        else:
            duration_s = 0.0
            fmt = payload.get("format", {})
            try:
                duration_s = float(fmt.get("duration", 0.0))
            except Exception:
                duration_s = 0.0
            if fps > 0 and duration_s > 0:
                frame_count = int(round(duration_s * fps))

        return VideoMetadata(
            name=self.name,
            path=path,
            width=width,
            height=height,
            fps=fps,
            frame_count=frame_count,
            codec=str(stream.get("codec_name", "unknown")),
        )

    def close(self) -> None:
        if self.process is None:
            return

        proc = self.process
        self.process = None

        try:
            proc.terminate()
            proc.wait(timeout=1.0)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

    def _spawn(self, start_frame: int) -> None:
        self.close()

        cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
        ]

        if self.hwaccel:
            cmd.extend(["-hwaccel", self.hwaccel])
        if self.hw_decoder:
            cmd.extend(["-c:v", self.hw_decoder])

        cmd.extend(["-i", self.path])

        if start_frame > 0:
            cmd.extend(["-vf", f"select='gte(n,{start_frame})'"])

        cmd.extend([
            "-an",
            "-sn",
            "-dn",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgba",
            "-vsync",
            "0",
            "-",
        ])

        logger.info("Starting ffmpeg stream for %s at frame %d", self.name, start_frame)
        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.current_frame = start_frame

    def _read_exact_frame(self) -> Optional[bytes]:
        if self.process is None or self.process.stdout is None:
            return None

        out = bytearray()
        while len(out) < self.frame_size_bytes:
            chunk = self.process.stdout.read(self.frame_size_bytes - len(out))
            if not chunk:
                break
            out.extend(chunk)

        if len(out) == 0:
            return None

        if len(out) != self.frame_size_bytes:
            stderr_tail = ""
            if self.process.stderr is not None:
                try:
                    stderr_tail = self.process.stderr.read().decode("utf-8", errors="replace")
                except Exception:
                    stderr_tail = ""
            raise RuntimeError(
                f"Incomplete frame read for {self.name}: expected {self.frame_size_bytes}, "
                f"got {len(out)}. ffmpeg stderr: {stderr_tail.strip()}"
            )

        return bytes(out)

    def get_frame(self, frame_index: int) -> bytes:
        if frame_index < 0:
            raise ValueError("frame index must be >= 0")

        if self.process is None or frame_index < self.current_frame:
            self._spawn(frame_index)

        while self.current_frame < frame_index:
            skipped = self._read_exact_frame()
            if skipped is None:
                raise IndexError(
                    f"Requested frame {frame_index}, but stream ended at frame {self.current_frame}."
                )
            self.current_frame += 1

        frame = self._read_exact_frame()
        if frame is None:
            raise IndexError(
                f"Requested frame {frame_index}, but stream ended at frame {self.current_frame}."
            )
        self.current_frame += 1
        return frame

    def to_dict(self) -> dict:
        return {
            "name": self.metadata.name,
            "path": self.metadata.path,
            "width": self.metadata.width,
            "height": self.metadata.height,
            "fps": self.metadata.fps,
            "frame_count": self.metadata.frame_count,
            "codec": self.metadata.codec,
            "current_frame": self.current_frame,
            "hwaccel": self.hwaccel,
            "hw_decoder": self.hw_decoder,
        }


class VideoRegistry:
    """Named video collection with persistent ffmpeg decode state."""

    def __init__(
        self,
        cache_dir: str,
        hwaccel: str = "auto",
        hw_decoder: Optional[str] = None,
    ) -> None:
        self.cache_dir = os.path.abspath(os.path.expanduser(cache_dir))
        self.hwaccel = hwaccel
        self.hw_decoder = hw_decoder
        self.videos: Dict[str, FFmpegVideoStream] = {}

        Path(self.cache_dir).mkdir(parents=True, exist_ok=True)

    def close(self) -> None:
        for stream in self.videos.values():
            stream.close()
        self.videos.clear()

    def load_video(
        self,
        name: str,
        path: str,
        hwaccel: Optional[str] = None,
        hw_decoder: Optional[str] = None,
    ) -> dict:
        if not name:
            raise ValueError("name is required")
        if not path:
            raise ValueError("path is required")

        resolved_path = os.path.abspath(os.path.expanduser(path))
        if not os.path.isfile(resolved_path):
            raise FileNotFoundError(f"Video file not found: {resolved_path}")

        if name in self.videos:
            self.videos[name].close()

        stream = FFmpegVideoStream(
            name=name,
            path=resolved_path,
            hwaccel=hwaccel or self.hwaccel,
            hw_decoder=hw_decoder or self.hw_decoder,
        )
        self.videos[name] = stream
        return stream.to_dict()

    def unload_video(self, name: str) -> bool:
        stream = self.videos.pop(name, None)
        if stream is None:
            return False
        stream.close()
        return True

    def list_videos(self) -> dict:
        return {name: stream.to_dict() for name, stream in self.videos.items()}

    def get_frame(self, name: str, frame_index: int) -> Tuple[int, int, bytes]:
        if name not in self.videos:
            raise KeyError(f"Video not loaded: {name}")

        stream = self.videos[name]
        frame = stream.get_frame(frame_index)
        return stream.metadata.width, stream.metadata.height, frame

    def load_videos_from_gcode(self, gcode_path: str) -> dict:
        if not gcode_path:
            raise ValueError("gcode_path is required")

        resolved = os.path.abspath(os.path.expanduser(gcode_path))
        if not os.path.isfile(resolved):
            raise FileNotFoundError(f"G-code file not found: {resolved}")

        extracted = extract_videos_from_gcode(resolved, self.cache_dir)
        loaded = {}
        for name, path in extracted.items():
            loaded[name] = self.load_video(name, path)
        return loaded
