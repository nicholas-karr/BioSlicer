"""Shared prerequisite discovery for BioSlicer integration tests."""

from __future__ import annotations

import os
import unittest
from pathlib import Path
from typing import Optional


REPO_ROOT = Path(__file__).resolve().parents[2]


def find_slicer_binary() -> Optional[Path]:
    configured = os.environ.get("BIOSLICER_BIN")
    candidates = []
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.extend(
        [
            REPO_ROOT / "build-default" / "src" / "prusa-slicer",
            REPO_ROOT / "build" / "src" / "prusa-slicer",
            REPO_ROOT / "build-default" / "src" / "prusa-slicer.exe",
            REPO_ROOT / "build" / "src" / "prusa-slicer.exe",
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def require_slicer_binary() -> Path:
    binary = find_slicer_binary()
    if binary is None:
        raise unittest.SkipTest(
            "BioSlicer integration test requires BIOSLICER_BIN or a binary "
            "under build-default/src or build/src"
        )
    return binary
