#!/usr/bin/env python3

import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "gen_channel_setup_override.py"


def read_option(path: Path, key: str) -> list[str]:
    prefix = f"{key} = "
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :].split(",")
    raise AssertionError(f"Missing option {key}")


class ChannelSetupOverrideTests(unittest.TestCase):
    def generate(self, channels: int, sla_channels: str) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        output = Path(temporary.name) / "override.ini"
        subprocess.run(
            [
                "python3",
                str(SCRIPT),
                "--output",
                str(output),
                "--channels",
                str(channels),
                "--sla-channels",
                sla_channels,
                "--embed-sla-video",
                "--synthesize-sla-video",
            ],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return output

    def test_single_channel_vector(self):
        output = self.generate(1, "1")
        self.assertEqual(read_option(output, "sla_material_extruder"), ["1"])

    def test_two_channel_vector(self):
        output = self.generate(2, "2")
        self.assertEqual(read_option(output, "sla_material_extruder"), ["0", "1"])

    def test_fourth_channel_is_a_boolean_position_not_a_value(self):
        output = self.generate(4, "4")
        self.assertEqual(
            read_option(output, "sla_material_extruder"), ["0", "0", "0", "1"]
        )

    def test_high_channel_vector_preserves_requested_indices(self):
        output = self.generate(100, "2,17,100")
        values = read_option(output, "sla_material_extruder")
        self.assertEqual(len(values), 100)
        self.assertEqual(
            [index + 1 for index, value in enumerate(values) if value == "1"],
            [2, 17, 100],
        )


if __name__ == "__main__":
    unittest.main()
