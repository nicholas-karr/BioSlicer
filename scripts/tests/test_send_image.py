#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from unittest import mock


def _load_send_image_module():
    module_path = Path(__file__).resolve().parents[1] / "send-image.py"
    spec = importlib.util.spec_from_file_location("send_image_script", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import module from {module_path}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


send_image = _load_send_image_module()


class SendImageServiceTests(unittest.TestCase):
    def test_start_service_uses_non_interactive_flag(self):
        run_result = mock.Mock(returncode=1, stderr="denied", stdout="")
        with mock.patch.object(send_image.subprocess, "run", return_value=run_result) as run_mock:
            started, detail = send_image._start_service("bioslicer-image-display.service", user_mode=False)

        self.assertFalse(started)
        self.assertIn("denied", detail)

        cmd = run_mock.call_args.args[0]
        self.assertIn("--no-ask-password", cmd)

    def test_ensure_service_running_prefers_user_mode(self):
        with mock.patch.object(send_image, "_systemctl_available", return_value=True), \
            mock.patch.dict(send_image.os.environ, {"XDG_RUNTIME_DIR": "/run/user/1000"}, clear=False), \
            mock.patch.object(send_image, "_service_active", side_effect=[False, True]), \
            mock.patch.object(send_image, "_start_service", return_value=(True, "")) as start_mock:
            ok, msg = send_image.ensure_service_running("bioslicer-image-display.service")

        self.assertTrue(ok)
        self.assertIn("user mode", msg)
        self.assertEqual(start_mock.call_count, 1)
        self.assertTrue(start_mock.call_args.args[1])

    def test_send_command_local_autostart_success(self):
        with mock.patch.object(send_image, "_send_once", side_effect=[ConnectionRefusedError(), {"status": "success"}]), \
            mock.patch.object(send_image, "ensure_service_running", return_value=(True, "started")) as ensure_mock:
            response = send_image.send_command(
                "localhost",
                5555,
                {"type": "CLEAR"},
                ensure_running=True,
                start_timeout=1.0,
            )

        self.assertEqual(response["status"], "success")
        ensure_mock.assert_called_once()

    def test_send_command_remote_refused_does_not_autostart(self):
        with mock.patch.object(send_image, "_send_once", side_effect=ConnectionRefusedError()), \
            mock.patch.object(send_image, "ensure_service_running") as ensure_mock:
            response = send_image.send_command(
                "192.168.0.2",
                5555,
                {"type": "CLEAR"},
                ensure_running=True,
            )

        self.assertEqual(response["status"], "error")
        self.assertIn("Connection refused", response["message"])
        ensure_mock.assert_not_called()

    def test_send_command_local_autostart_failure_returns_detail(self):
        with mock.patch.object(send_image, "_send_once", side_effect=ConnectionRefusedError()), \
            mock.patch.object(send_image, "ensure_service_running", return_value=(False, "unit missing")):
            response = send_image.send_command(
                "localhost",
                5555,
                {"type": "CLEAR"},
                ensure_running=True,
            )

        self.assertEqual(response["status"], "error")
        self.assertIn("Autostart failed", response["message"])
        self.assertIn("unit missing", response["message"])


if __name__ == "__main__":
    unittest.main()