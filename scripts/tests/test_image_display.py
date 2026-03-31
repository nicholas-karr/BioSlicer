#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import types
import unittest
from pathlib import Path
from unittest import mock


def _load_image_display_module():
    module_path = Path(__file__).resolve().parents[1] / "image-display.py"

    pyglet_stub = types.ModuleType("pyglet")
    pyglet_stub.gl = types.SimpleNamespace(Config=lambda *args, **kwargs: object())

    class _DummyWindow:
        def __init__(self, *args, **kwargs):
            pass

    pyglet_stub.window = types.SimpleNamespace(Window=_DummyWindow)
    pyglet_stub.sprite = types.SimpleNamespace(Sprite=lambda *args, **kwargs: object())
    pyglet_stub.image = types.SimpleNamespace(ImageData=lambda *args, **kwargs: object())
    pyglet_stub.clock = types.SimpleNamespace(schedule_once=lambda *args, **kwargs: None)
    pyglet_stub.app = types.SimpleNamespace(run=lambda: None)

    pil_stub = types.ModuleType("PIL")
    pil_image_stub = types.ModuleType("PIL.Image")
    pil_stub.Image = pil_image_stub

    numpy_stub = types.ModuleType("numpy")
    numpy_stub.array = lambda x: x
    numpy_stub.flipud = lambda x: x
    numpy_stub.frombuffer = lambda *args, **kwargs: b""
    numpy_stub.uint8 = int

    yaml_stub = types.ModuleType("yaml")
    yaml_stub.safe_load = lambda *args, **kwargs: {}

    runtime_stub = types.ModuleType("sla_video_runtime")

    class _DummyVideoRegistry:
        def __init__(self, *args, **kwargs):
            pass

        def close(self):
            pass

    runtime_stub.VideoRegistry = _DummyVideoRegistry

    stub_modules = {
        "pyglet": pyglet_stub,
        "PIL": pil_stub,
        "PIL.Image": pil_image_stub,
        "numpy": numpy_stub,
        "yaml": yaml_stub,
        "sla_video_runtime": runtime_stub,
    }

    with mock.patch.dict(sys.modules, stub_modules, clear=False):
        spec = importlib.util.spec_from_file_location("image_display_script", module_path)
        if spec is None or spec.loader is None:
            raise RuntimeError(f"Unable to import module from {module_path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


image_display = _load_image_display_module()


class _FakeScreen:
    def __init__(self, width: int, height: int, x: int, y: int):
        self.width = width
        self.height = height
        self.x = x
        self.y = y


class _FakeDisplay:
    def __init__(self, screens):
        self._screens = list(screens)

    def get_screens(self):
        return list(self._screens)


class FindMonitorTests(unittest.TestCase):
    def setUp(self):
        self.screen0 = _FakeScreen(1920, 1080, 0, 0)
        self.screen1 = _FakeScreen(1280, 720, 1920, 0)
        self.display = _FakeDisplay([self.screen0, self.screen1])

    def test_invalid_monitor_index_falls_back_to_zero(self):
        selected = image_display.find_monitor(
            self.display,
            monitor_index="bad",
            monitor_auto_detect=False,
        )
        self.assertIs(selected, self.screen0)

    def test_negative_monitor_index_falls_back_to_zero(self):
        selected = image_display.find_monitor(
            self.display,
            monitor_index=-2,
            monitor_auto_detect=False,
        )
        self.assertIs(selected, self.screen0)

    def test_auto_detect_uses_guess_when_index_not_set(self):
        with mock.patch.object(image_display, "guess_projector_monitor", return_value=self.screen1) as guess_mock:
            selected = image_display.find_monitor(
                self.display,
                monitor_index=None,
                monitor_auto_detect=True,
            )
        self.assertIs(selected, self.screen1)
        guess_mock.assert_called_once()

    def test_size_and_position_match_takes_priority(self):
        selected = image_display.find_monitor(
            self.display,
            monitor_size=(1280, 720),
            monitor_position=(1920, 0),
            monitor_auto_detect=True,
        )
        self.assertIs(selected, self.screen1)


class CacheDirDefaultTests(unittest.TestCase):
    def test_cache_dir_prefers_env_override(self):
        with mock.patch.dict(image_display.os.environ, {"BIOSLICER_CACHE_ROOT": "/custom/cache"}, clear=False), \
            mock.patch.object(image_display.os.path, "isdir", side_effect=lambda p: p == "/custom/cache"):
            cache_dir = image_display.default_video_cache_dir()

        self.assertEqual(cache_dir, "/custom/cache/bioslicer-sla-video-cache")

    def test_cache_dir_falls_back_to_tmp(self):
        with mock.patch.dict(image_display.os.environ, {}, clear=True), \
            mock.patch.object(image_display.os.path, "isdir", return_value=False):
            cache_dir = image_display.default_video_cache_dir()

        self.assertEqual(cache_dir, "/tmp/bioslicer-sla-video-cache")


if __name__ == "__main__":
    unittest.main()