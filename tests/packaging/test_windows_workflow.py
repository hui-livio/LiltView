"""Windows release command contracts, including native PowerShell argument passing."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def windows_configure_command():
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    windows = workflow.split("\n  windows:", 1)[1].split("\n  macos:", 1)[0]
    return next(line.strip() for line in windows.splitlines()
                if line.strip().startswith("cmake --preset "))


class WindowsWorkflowTests(unittest.TestCase):
    def test_compiler_definitions_are_quoted_for_powershell(self):
        command = windows_configure_command()
        self.assertIn('"-DCMAKE_C_COMPILER=cl.exe"', command)
        self.assertIn('"-DCMAKE_CXX_COMPILER=cl.exe"', command)

    @unittest.skipUnless(shutil.which("pwsh"), "requires PowerShell 7")
    def test_configure_command_passes_intact_native_arguments(self):
        # Run the real workflow command through PowerShell, substituting a native
        # argv probe for CMake. A PowerShell function mock can parse differently.
        with tempfile.TemporaryDirectory(prefix="liltview argument probe ") as directory:
            probe = Path(directory) / "native argv.py"
            probe.write_text("import json, sys; print(json.dumps(sys.argv[1:]))\n", encoding="utf-8")
            command = windows_configure_command().replace(
                "cmake ", '& $env:LILTVIEW_PROBE_PYTHON $env:LILTVIEW_PROBE_SCRIPT ', 1)
            script = Path(directory) / "configure probe.ps1"
            script.write_text("$ErrorActionPreference = 'Stop'\n" + command + "\nexit $LASTEXITCODE\n",
                              encoding="utf-8")
            for suffix in ("-rc.1", ""):
                with self.subTest(suffix=suffix):
                    env = dict(os.environ, VERSION_SUFFIX=suffix,
                               LILTVIEW_PROBE_PYTHON=sys.executable, LILTVIEW_PROBE_SCRIPT=str(probe))
                    result = subprocess.run(
                        [shutil.which("pwsh"), "-NoProfile", "-NonInteractive", "-File", str(script)],
                        env=env, cwd=directory, capture_output=True, text=True, encoding="utf-8", timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(json.loads(result.stdout), [
                        "--preset", "release-ci", "-DCMAKE_C_COMPILER=cl.exe",
                        "-DCMAKE_CXX_COMPILER=cl.exe", f"-DLILTVIEW_VERSION_SUFFIX={suffix}"])


if __name__ == "__main__":
    unittest.main()
