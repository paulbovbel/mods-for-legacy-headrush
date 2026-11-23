"""Offline CLI tests using synthetic files, never vendor firmware or hardware.

Run: python3 tests/test_file_plan.py /path/to/gui-core-cli
"""

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CLI = os.environ.get("NAM_PLAN_CLI")
if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    CLI = str(Path(sys.argv[1]).resolve())


def synthetic_evil():
    """Minimal ARM ELF with the target's expected slots and a zero code cave."""
    data = bytearray(0x17EE600 - 0x8000)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HH", data, 16, 2, 40)
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<HHH", data, 40, 52, 32, 2)
    struct.pack_into("<8I", data, 52, 1, 0, 0x8000, 0x8000, 0x100, 0x100, 5, 4)
    struct.pack_into(
        "<8I", data, 84, 1, 0x200, 0x8200, 0x8200,
        len(data) - 0x200, len(data) - 0x200, 6, 4,
    )
    for table in (0x17EE460, 0x17EE4DC):
        struct.pack_into("<I", data, table + 32 - 0x8000, 0x302ED0)
    return data


@unittest.skipUnless(CLI, "set NAM_PLAN_CLI to the built gui-core-cli")
class FilePlanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.input = self.root / "input"
        self.evil = self.input / "usr/Evil/Evil"
        self.launcher = self.input / "usr/Evil/Scripts/evil"
        self.launcher.parent.mkdir(parents=True)
        self.evil.write_bytes(synthetic_evil())
        self.launcher.write_text(
            "#!/bin/sh\nwhile [ 1 ]; do\n"
            "\tsystemd-inhibit --what=handle-power-key /usr/Evil/Evil\n"
            "\texitCode=$?\ndone\n"
        )
        self.output = self.root / "plan"

    def run_cli(self, *options, success=True):
        result = subprocess.run(
            [CLI, str(self.input), str(self.output), "--file-plan", "--model", "mx5", *options],
            check=False, capture_output=True, text=True,
            env={**os.environ, "NAM_DUMP_EVIL": str(self.root / "unexpected-dump")},
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "unexpected-dump").exists())
        return result

    def test_two_and_four_instance_plans(self):
        originals = [hashlib.sha256(p.read_bytes()).digest() for p in (self.evil, self.launcher)]
        for instances in (2, 4):
            with self.subTest(instances=instances):
                self.output = self.root / f"plan-{instances}"
                result = self.run_cli("--instances", str(instances))
                self.assertNotIn("working directory:", result.stdout)
                manifest = json.loads((self.output / "plan.json").read_text())
                self.assertEqual(manifest["version"], 1)
                entries = manifest["files"]
                self.assertEqual(len(entries), 4)
                plan = {
                    entry["path"]: ((self.output / entry["file"]).read_bytes(), entry["mode"])
                    for entry in entries
                }
                self.assertEqual(set(plan), {
                    "/usr/Evil/Evil", "/usr/Evil/Scripts/evil",
                    "/usr/Evil/libnam_hook.so", "/usr/Evil/libnam_preload.so",
                })
                self.assertTrue(all(data and mode == 0o100755 for data, mode in plan.values()))
                script = plan["/usr/Evil/Scripts/evil"][0]
                self.assertIn(b"LD_PRELOAD=/usr/Evil/libnam_preload.so", script)
                self.assertEqual(b"NAM_HOOK_SLOT_GONK_V2_ADDR=" in script, instances == 4)
                patched = plan["/usr/Evil/Evil"][0]
                self.assertEqual(struct.unpack_from("<I", patched, 0x17EE480 - 0x8000)[0], 0x8100)
                self.assertEqual(
                    struct.unpack_from("<I", patched, 0x17EE4FC - 0x8000)[0],
                    0x811C if instances == 4 else 0x302ED0,
                )
                for path in ("/usr/Evil/libnam_hook.so", "/usr/Evil/libnam_preload.so"):
                    self.assertEqual(plan[path][0][:4], b"\x7fELF")
        self.assertEqual(
            originals, [hashlib.sha256(p.read_bytes()).digest() for p in (self.evil, self.launcher)],
        )

    def test_rejects_page_turner_before_reading_files(self):
        self.evil.unlink()
        result = self.run_cli("--page-pedal", success=False)
        self.assertIn("page-turner is unsupported", result.stderr)
        self.assertFalse(self.output.exists())

    def test_rejects_unknown_model(self):
        result = self.run_cli("--model", "unknown", success=False)
        self.assertIn("unknown --model", result.stderr)
        self.assertFalse(self.output.exists())

    def test_rejects_invalid_instances(self):
        self.run_cli("--instances", "3", success=False)
        self.assertFalse(self.output.exists())

    def test_rejects_bad_elf(self):
        self.evil.write_bytes(b"not an ELF")
        self.run_cli(success=False)
        self.assertFalse(self.output.exists())

    def test_rejects_bad_launcher(self):
        for script in (b"no launch point", b"while [ 1 ]\n\x00hidden"):
            self.launcher.write_bytes(script)
            self.run_cli(success=False)
            self.assertFalse(self.output.exists())

    def test_rejects_missing_input(self):
        self.launcher.unlink()
        self.run_cli(success=False)
        self.assertFalse(self.output.exists())

    def test_refuses_existing_output(self):
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_text("unchanged")
        self.run_cli(success=False)
        self.assertEqual(list(self.output.iterdir()), [marker])
        self.assertEqual(marker.read_text(), "unchanged")

    def test_rejects_already_patched_input(self):
        self.run_cli()
        manifest = json.loads((self.output / "plan.json").read_text())
        entry = next(e for e in manifest["files"] if e["path"] == "/usr/Evil/Evil")
        self.evil.write_bytes((self.output / entry["file"]).read_bytes())
        self.output = self.root / "second-plan"
        result = self.run_cli(success=False)
        self.assertIn("REFUSING", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
