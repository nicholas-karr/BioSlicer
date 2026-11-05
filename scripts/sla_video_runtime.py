#!/usr/bin/env python3
"""Parse BioSlicer SLA-video metadata and extract embedded payloads.

This module intentionally contains no video playback or display runtime.  It is
the small, dependency-free boundary needed by extraction tools and automated
tests for the G-code format emitted by BioSlicer.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import os
import re
import tempfile
from pathlib import Path
from typing import Dict, Optional, TextIO


_SAFE_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_BEGIN_RE = re.compile(
    r"^;\s*bioslicer_sla_video begin "
    r"name=(\S+) extruder=(\d+) bytes=(\d+) sha256=([0-9a-fA-F]{64})\s*$"
)
_DATA_RE = re.compile(r"^;\s*bioslicer_sla_video ([A-Za-z0-9+/=]+)\s*$")
_END_RE = re.compile(r"^;\s*bioslicer_sla_video end name=(\S+)\s*$")
_REF_RE = re.compile(
    r"^;\s*bioslicer_sla_video ref "
    r"name=(\S+) extruder=(\d+) (path|path_b64)=(.*)\s*$"
)


def safe_video_name(name: str) -> str:
    """Validate and return a video identifier safe for use as one path segment."""
    if not _SAFE_NAME_RE.fullmatch(name) or name in {".", ".."}:
        raise RuntimeError(f"Unsafe SLA video name: {name!r}")
    return name


class _EmbeddedPayload:
    def __init__(
        self,
        output_dir: Path,
        name: str,
        expected_bytes: int,
        expected_sha256: str,
        max_embedded_bytes: Optional[int],
    ) -> None:
        self.name = safe_video_name(name)
        self.expected_bytes = expected_bytes
        self.expected_sha256 = expected_sha256.lower()
        if max_embedded_bytes is not None and expected_bytes > max_embedded_bytes:
            raise RuntimeError(
                f"Embedded SLA video {name!r} declares {expected_bytes} bytes, "
                f"exceeding the configured limit of {max_embedded_bytes}"
            )

        temporary = tempfile.NamedTemporaryFile(
            mode="w+b",
            prefix=f".{self.name}.",
            suffix=".part",
            dir=output_dir,
            delete=False,
        )
        self.stream = temporary
        self.temporary_path = Path(temporary.name)
        self.hasher = hashlib.sha256()
        self.bytes_written = 0
        self.base64_carry = ""

    def append_base64(self, encoded: str) -> None:
        self.base64_carry += encoded
        decode_length = len(self.base64_carry) - (len(self.base64_carry) % 4)
        if decode_length == 0:
            return
        self._decode_and_write(self.base64_carry[:decode_length])
        self.base64_carry = self.base64_carry[decode_length:]

    def _decode_and_write(self, encoded: str) -> None:
        try:
            decoded = base64.b64decode(encoded, validate=True)
        except binascii.Error as exc:
            raise RuntimeError(
                f"Invalid base64 in embedded SLA video {self.name!r}"
            ) from exc
        self.bytes_written += len(decoded)
        if self.bytes_written > self.expected_bytes:
            raise RuntimeError(
                f"Embedded SLA video {self.name!r} exceeds its declared length"
            )
        self.stream.write(decoded)
        self.hasher.update(decoded)

    def finish(self, output_dir: Path) -> Path:
        if self.base64_carry:
            if len(self.base64_carry) % 4:
                raise RuntimeError(
                    f"Truncated base64 in embedded SLA video {self.name!r}"
                )
            self._decode_and_write(self.base64_carry)
            self.base64_carry = ""

        self.stream.flush()
        os.fsync(self.stream.fileno())
        self.stream.close()

        if self.bytes_written != self.expected_bytes:
            raise RuntimeError(
                f"Embedded SLA video {self.name!r} has {self.bytes_written} bytes; "
                f"expected {self.expected_bytes}"
            )
        actual_sha256 = self.hasher.hexdigest()
        if actual_sha256 != self.expected_sha256:
            raise RuntimeError(
                f"Embedded SLA video {self.name!r} has SHA-256 {actual_sha256}; "
                f"expected {self.expected_sha256}"
            )

        destination = output_dir / f"{self.name}-{actual_sha256[:12]}.mkv"
        os.replace(self.temporary_path, destination)
        return destination

    def discard(self) -> None:
        try:
            self.stream.close()
        finally:
            self.temporary_path.unlink(missing_ok=True)


def _decode_reference_path(kind: str, value: str, name: str) -> str:
    if kind == "path":
        if "\x00" in value:
            raise RuntimeError(f"NUL byte in SLA video reference {name!r}")
        return value
    try:
        raw = base64.b64decode(value, validate=True)
        path = raw.decode("utf-8")
    except (binascii.Error, UnicodeDecodeError) as exc:
        raise RuntimeError(
            f"Invalid path_b64 in SLA video reference {name!r}"
        ) from exc
    if "\x00" in path:
        raise RuntimeError(f"NUL byte in SLA video reference {name!r}")
    return path


def extract_videos_from_gcode(
    gcode_path: str,
    output_dir: str,
    *,
    max_embedded_bytes: Optional[int] = None,
) -> Dict[str, str]:
    """Extract embedded videos and return all video names mapped to paths.

    Embedded data is decoded incrementally, verified, fsynced, and atomically
    published. References support both the legacy plain ``path=`` form and the
    injection-safe ``path_b64=`` form.
    """
    source = Path(gcode_path)
    destination_dir = Path(output_dir)
    destination_dir.mkdir(parents=True, exist_ok=True)

    result: Dict[str, str] = {}
    current: Optional[_EmbeddedPayload] = None

    try:
        with source.open("r", encoding="utf-8", errors="strict") as gcode:
            for line_number, line in enumerate(gcode, 1):
                line = line.rstrip("\r\n")

                if current is not None:
                    end_match = _END_RE.match(line)
                    if end_match:
                        end_name = safe_video_name(end_match.group(1))
                        if end_name != current.name:
                            raise RuntimeError(
                                f"SLA video end name {end_name!r} does not match "
                                f"{current.name!r} at line {line_number}"
                            )
                        result[current.name] = str(current.finish(destination_dir))
                        current = None
                        continue

                    data_match = _DATA_RE.match(line)
                    if not data_match:
                        raise RuntimeError(
                            f"Unexpected line inside embedded SLA video "
                            f"{current.name!r} at line {line_number}"
                        )
                    current.append_base64(data_match.group(1))
                    continue

                begin_match = _BEGIN_RE.match(line)
                if begin_match:
                    name = safe_video_name(begin_match.group(1))
                    if name in result:
                        raise RuntimeError(f"Duplicate SLA video name: {name!r}")
                    current = _EmbeddedPayload(
                        destination_dir,
                        name,
                        int(begin_match.group(3)),
                        begin_match.group(4),
                        max_embedded_bytes,
                    )
                    continue

                reference_match = _REF_RE.match(line)
                if reference_match:
                    name = safe_video_name(reference_match.group(1))
                    if name in result:
                        raise RuntimeError(f"Duplicate SLA video name: {name!r}")
                    result[name] = _decode_reference_path(
                        reference_match.group(3),
                        reference_match.group(4),
                        name,
                    )

        if current is not None:
            raise RuntimeError(f"Unterminated embedded SLA video {current.name!r}")
    except Exception:
        if current is not None:
            current.discard()
        raise

    return result
