"""Exercise the actual PKGBUILD with shell stubs, without compiling dependencies."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "requires POSIX bash")
class ArchRecipeTests(unittest.TestCase):
    def run_recipe(self, tests=None, ctest_status=0):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = root / "build/app/liltview"
            app.parent.mkdir(parents=True)
            app.write_text('#!/bin/bash\nprintf "APP %s\\n" "$*"\n', encoding="utf-8")
            app.chmod(0o755)
            environment = os.environ.copy()
            environment.pop("LILTVIEW_RUN_TESTS", None)
            environment.pop("LILTVIEW_RELEASE_VERSION", None)
            environment.update(LILTVIEW_SOURCE_DIR=str(ROOT), RECIPE=str(ROOT / "packaging/arch/PKGBUILD"),
                               startdir=str(ROOT / "packaging/arch"), srcdir=str(root),
                               CTEST_STATUS=str(ctest_status), CFLAGS="", CXXFLAGS="")
            if tests is not None:
                environment["LILTVIEW_RUN_TESTS"] = tests
            # errexit matches makepkg's error handling: a failing UT must abort packaging.
            return subprocess.run(["bash", "-eu", "-c", r'''
cmake() { printf 'CMAKE %s\n' "$*"; }
ctest() { printf 'CTEST %s\n' "$*"; return "$CTEST_STATUS"; }
desktop-file-validate() { printf 'DESKTOP %s\n' "$*"; }
source "$RECIPE"
build
check
printf 'CHECK_COMPLETED\n'
'''], env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_default_install_neither_builds_nor_runs_tests(self):
        result = self.run_recipe(ctest_status=23)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", result.stdout)
        self.assertIn("-DBUILD_TESTING=OFF", result.stdout)
        self.assertIn("-DLILTVIEW_BUILD_UNIT_TESTS=OFF", result.stdout)
        self.assertNotIn("CTEST ", result.stdout)
        self.assertIn("APP --version", result.stdout)
        self.assertIn("DESKTOP ", result.stdout)

    def test_explicit_opt_in_builds_and_runs_release_tests(self):
        result = self.run_recipe("1")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("-DBUILD_TESTING=ON", result.stdout)
        self.assertIn("-DLILTVIEW_BUILD_UNIT_TESTS=ON", result.stdout)
        self.assertIn("CTEST --test-dir", result.stdout)
        self.assertIn("--build-config Release", result.stdout)
        self.assertIn("--no-tests=error", result.stdout)

    def test_explicit_opt_out_turns_tests_off(self):
        result = self.run_recipe("0")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("-DBUILD_TESTING=OFF", result.stdout)
        self.assertNotIn("CTEST ", result.stdout)

    def test_ci_test_failure_still_aborts_packaging(self):
        result = self.run_recipe("1", ctest_status=23)
        self.assertEqual(result.returncode, 23, result.stdout)
        self.assertNotIn("CHECK_COMPLETED", result.stdout)

    def test_invalid_switch_is_rejected(self):
        result = self.run_recipe("yes")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("LILTVIEW_RUN_TESTS must be 0 or 1", result.stdout)
        self.assertNotIn("CMAKE ", result.stdout)

    def test_package_does_not_case_only_rename_licenses(self):
        recipe = (ROOT / "packaging/arch/PKGBUILD").read_text(encoding="utf-8")
        self.assertNotIn('mv "${pkgdir}/usr/share/licenses/', recipe)
        self.assertIn('"${pkgdir}/usr/share/licenses/${pkgname}/HarmonyOS-Sans/LICENSE.txt"', recipe)

    def test_ci_explicitly_enables_tests(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn('runuser -u builder -- python3 "$PWD/src/build.py" arch --parallel 3', workflow)


if __name__ == "__main__":
    unittest.main()
