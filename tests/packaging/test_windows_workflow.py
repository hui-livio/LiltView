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


def windows_build_command():
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    windows = workflow.split("\n  windows:", 1)[1].split("\n  macos:", 1)[0]
    return next(line.strip() for line in windows.splitlines()
                if line.strip().startswith("python build.py build "))


class WindowsWorkflowTests(unittest.TestCase):
    def test_workflow_delegates_build_and_tests_to_python(self):
        self.assertEqual(windows_build_command(), "python build.py build --parallel 4")
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        windows = workflow.split("\n  windows:", 1)[1].split("\n  macos:", 1)[0]
        self.assertIn("python build.py test --test-parallel 1", windows)
        self.assertNotIn("cmake --preset", windows)
        self.assertIn("actions/setup-python@", windows)

    @unittest.skipUnless(shutil.which("pwsh"), "requires PowerShell 7")
    def test_workflow_command_preserves_compiler_and_version_arguments(self):
        # Exercise the actual PowerShell workflow line and Python CLI together.
        # Record the resulting CMake argument lists without compiling dependencies.
        with tempfile.TemporaryDirectory(prefix="liltview argument probe ") as directory:
            probe = Path(directory) / "native argv.py"
            probe.write_text(
                "import importlib.util, json, os\n"
                "spec = importlib.util.spec_from_file_location('builder', os.environ['LILTVIEW_PROBE_BUILD'])\n"
                "builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)\n"
                "builder.platform.system = lambda: 'Windows'\n"
                "calls = []; builder.run = lambda command, **kwargs: calls.append(command)\n"
                "builder.main(); print(json.dumps(calls))\n", encoding="utf-8")
            command = windows_build_command().replace(
                "python build.py ", '& $env:LILTVIEW_PROBE_PYTHON $env:LILTVIEW_PROBE_SCRIPT ', 1)
            script = Path(directory) / "configure probe.ps1"
            script.write_text("$ErrorActionPreference = 'Stop'\n" + command + "\nexit $LASTEXITCODE\n",
                              encoding="utf-8")
            for suffix in ("-rc.1", ""):
                with self.subTest(suffix=suffix):
                    env = dict(os.environ, VERSION_SUFFIX=suffix, CC="cl.exe", CXX="cl.exe",
                               LILTVIEW_PROBE_PYTHON=sys.executable, LILTVIEW_PROBE_SCRIPT=str(probe),
                               LILTVIEW_PROBE_BUILD=str(ROOT / "build.py"))
                    result = subprocess.run(
                        [shutil.which("pwsh"), "-NoProfile", "-NonInteractive", "-File", str(script)],
                        env=env, cwd=directory, capture_output=True, text=True, encoding="utf-8", timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    configure, build = json.loads(result.stdout)
                    self.assertEqual(configure[:3], ["cmake", "--preset", "release-ci"])
                    self.assertIn("-DCMAKE_C_COMPILER=cl.exe", configure)
                    self.assertIn("-DCMAKE_CXX_COMPILER=cl.exe", configure)
                    self.assertIn(f"-DLILTVIEW_VERSION_SUFFIX={suffix}", configure)
                    self.assertNotIn(".exe", configure)
                    self.assertEqual(build[-2:], ["--parallel", "4"])


if __name__ == "__main__":
    unittest.main()
