"""Unified builder contracts; no dependency compilation is required."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("builder", ROOT / "build.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict(os.environ, {}, clear=False)
        self.environment.start()
        for name in ("VERSION_SUFFIX", "CMAKE_BUILD_PARALLEL_LEVEL"):
            os.environ.pop(name, None)
        self.addCleanup(self.environment.stop)
        self.platform = patch.object(builder, "platform_definitions", return_value=[])
        self.platform.start()
        self.addCleanup(self.platform.stop)

    def test_default_configures_builds_and_tests_in_release_order(self):
        with patch.object(builder, "run") as run:
            self.assertEqual(builder.main(["--parallel", "3"]), 0)
        configure, build, test = [call.args[0] for call in run.call_args_list]
        self.assertEqual(configure[:3], ["cmake", "--preset", "release-ci"])
        self.assertEqual(build[0:2], ["cmake", "--build"])
        self.assertIn("MinSizeRel", build)
        self.assertEqual(build[-2:], ["--parallel", "3"])
        self.assertEqual(test[0], "ctest")
        self.assertIn("MinSizeRel", test)
        self.assertIn("--no-tests=error", test)
        self.assertEqual(test[test.index("--parallel") + 1], "1" if os.name == "nt" else "2")
        self.assertEqual(configure[4], build[2])
        self.assertEqual(build[2], test[2])

    def test_application_only_build_does_not_run_ut(self):
        for preset in ("app-release", "app-minirel"):
            with self.subTest(preset=preset), patch.object(builder, "run") as run:
                builder.main(["build", "--preset", preset])
                self.assertEqual(run.call_count, 2)
                self.assertIn(preset, run.call_args_list[0].args[0])
                self.assertIn("MinSizeRel", run.call_args_list[1].args[0])

    def test_version_and_cache_values_remain_single_arguments(self):
        with patch.dict(os.environ, VERSION_SUFFIX="-rc.2"), patch.object(builder, "run") as run:
            builder.main(["configure", "--build-dir", "build/path with spaces", "-DRESOURCE_PATH=C:/source files"])
        command = run.call_args.args[0]
        self.assertIn("-DLILTVIEW_VERSION_SUFFIX=-rc.2", command)
        self.assertIn("-DRESOURCE_PATH=C:/source files", command)
        self.assertEqual(command[4], str(Path("build/path with spaces").resolve()))
        for suffix in ("", "-rc.3"):
            with self.subTest(suffix=suffix), patch.object(builder, "run") as run:
                builder.main(["configure", "--version-suffix=" + suffix])
                self.assertIn("-DLILTVIEW_VERSION_SUFFIX=" + suffix, run.call_args.args[0])

    def test_test_action_does_not_reconfigure_and_writes_junit(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(builder, "run") as run:
            report = Path(directory) / "reports/result.xml"
            builder.main(["test", "--test-parallel", "1", "--junit", str(report)])
            command = run.call_args.args[0]
            self.assertEqual(run.call_count, 1)
            self.assertEqual(command[0], "ctest")
            self.assertEqual(command[-2:], ["--output-junit", str(report.resolve())])
            self.assertTrue(report.parent.is_dir())

    def test_failed_configuration_stops_build_and_ut(self):
        failure = subprocess.CalledProcessError(23, ["cmake"])
        with patch.object(builder, "run", side_effect=failure) as run:
            with self.assertRaises(subprocess.CalledProcessError) as caught:
                builder.main([])
        self.assertEqual(caught.exception.returncode, 23)
        self.assertEqual(run.call_count, 1)

    def test_arch_delegates_to_native_recipe_with_release_ut_enabled(self):
        with patch.dict(os.environ, VERSION_SUFFIX="-rc.1"), patch.object(builder, "run") as run:
            builder.main(["arch", "--parallel", "3"])
        self.assertEqual(run.call_args.args[0], ["makepkg", "--check", "--noconfirm"])
        self.assertEqual(run.call_args.kwargs["cwd"], ROOT / "packaging/arch")
        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["LILTVIEW_RUN_TESTS"], "1")
        self.assertEqual(environment["CMAKE_BUILD_PARALLEL_LEVEL"], "3")
        self.assertEqual(environment["LILTVIEW_RELEASE_VERSION"], "1.0.0-rc.1")

    def test_invalid_arguments_fail_before_invoking_tools(self):
        for arguments in (["--parallel", "0"], ["--test-parallel", "-1"], ["-DBROKEN"],
                          ["-D=value"], ["test", "--preset", "app-release"],
                          ["test", "--preset", "app-minirel"],
                          ["arch", "--build-dir", "custom"]):
            with self.subTest(arguments=arguments), patch.object(builder, "run") as run:
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                    builder.main(arguments)
                self.assertEqual(caught.exception.code, 2)
                run.assert_not_called()

    def test_subprocesses_use_checked_argument_lists_without_a_shell(self):
        command = ["cmake", "-DCMAKE_C_COMPILER=C:/Program Files/compiler/cl.exe"]
        with patch.object(builder.subprocess, "run") as run, contextlib.redirect_stdout(io.StringIO()):
            builder.run(command)
        run.assert_called_once_with(command, cwd=ROOT, env=None, check=True)

    def test_every_binary_ci_job_uses_the_python_entry_point(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("      - 'build.py'", workflow)
        for name, next_name in (("windows", "macos"), ("macos", "linux"), ("linux", "arch"), ("arch", "release")):
            job = workflow.split("\n  " + name + ":", 1)[1].split("\n  " + next_name + ":", 1)[0]
            self.assertIn("build.py", job)
            self.assertNotIn("cmake --preset", job)
            self.assertNotIn("ctest --preset", job)


class CompilerSelectionTests(unittest.TestCase):
    def test_windows_defaults_and_environment_overrides(self):
        with patch.object(builder.platform, "system", return_value="Windows"), patch.dict(os.environ, {}, clear=True):
            self.assertEqual(builder.platform_definitions(),
                             ["-DCMAKE_C_COMPILER=cl.exe", "-DCMAKE_CXX_COMPILER=cl.exe"])
            with patch.dict(os.environ, CC="C:/Compiler Path/clang-cl.exe", CXX="clang-cl.exe"):
                self.assertIn("-DCMAKE_C_COMPILER=C:/Compiler Path/clang-cl.exe", builder.platform_definitions())

    def test_macos_preserves_llvm_c_apple_cpp_and_objc_toolchain(self):
        output = {("brew", "--prefix", "llvm@20"): "/opt/Homebrew Path/llvm@20",
                  ("xcrun", "-f", "clang++"): "/Xcode Path/clang++",
                  ("xcrun", "-f", "clang"): "/Xcode Path/clang",
                  ("xcrun", "--show-sdk-path"): "/Xcode Path/MacOSX.sdk"}
        with patch.object(builder.platform, "system", return_value="Darwin"), \
                patch.dict(os.environ, {"MACOSX_DEPLOYMENT_TARGET": "15.0"}, clear=True), \
                patch.object(builder, "tool_output", side_effect=lambda *args: output[args]):
            self.assertEqual(builder.platform_definitions(), [
                "-DCMAKE_C_COMPILER=/opt/Homebrew Path/llvm@20/bin/clang",
                "-DCMAKE_CXX_COMPILER=/Xcode Path/clang++", "-DCMAKE_OBJC_COMPILER=/Xcode Path/clang",
                "-DCMAKE_OSX_SYSROOT=/Xcode Path/MacOSX.sdk", "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0"])

    def test_linux_uses_the_distribution_compiler_selection(self):
        with patch.object(builder.platform, "system", return_value="Linux"):
            self.assertEqual(builder.platform_definitions(), [])


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "requires CMake and Ninja")
class OfflineBuilderTests(unittest.TestCase):
    def test_all_release_presets_select_minsizerel_and_ci_keeps_full_ut(self):
        with tempfile.TemporaryDirectory(prefix="liltview presets ") as directory:
            root = Path(directory)
            presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
            # No compiler is used: exercise Windows preset inheritance on any host.
            for preset in presets["configurePresets"]:
                if preset["name"] == "msvc-base":
                    preset["condition"] = {"type": "const", "value": True}
            (root / "CMakePresets.json").write_text(json.dumps(presets), encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.30)\nproject(Presets LANGUAGES NONE)\n', encoding="utf-8")
            for preset in ("release", "app-release", "app-minirel", "msvc-release", "release-ci"):
                with self.subTest(preset=preset):
                    result = subprocess.run(["cmake", "--preset", preset], cwd=root,
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    cache = (root / "build" / preset / "CMakeCache.txt").read_text(encoding="utf-8")
                    self.assertRegex(cache, r"(?m)^CMAKE_BUILD_TYPE:[A-Z]+=MinSizeRel$")
                    if preset == "release-ci":
                        for name in ("BUILD_TESTING", "LILTVIEW_BUILD_UNIT_TESTS", "LILTVIEW_RELEASE_PACKAGING"):
                            self.assertRegex(cache, rf"(?m)^{name}:BOOL=(TRUE|ON)$")

    def test_real_configure_build_and_test_from_git_free_source_with_spaces(self):
        with tempfile.TemporaryDirectory(prefix="liltview source ") as directory:
            root = Path(directory)
            shutil.copyfile(ROOT / "build.py", root / "build.py")
            # Exercise actual preset inheritance together with the Python entry point.
            shutil.copyfile(ROOT / "CMakePresets.json", root / "CMakePresets.json")
            environment = dict(os.environ, VERSION_SUFFIX="-rc.1")
            for succeeds in (True, False):
                with self.subTest(succeeds=succeeds):
                    (root / "CMakeLists.txt").write_text(
                        'cmake_minimum_required(VERSION 3.30)\nproject(Smoke LANGUAGES NONE)\n'
                        'enable_testing()\nadd_test(NAME smoke COMMAND "${CMAKE_COMMAND}" -E '
                        + ("true" if succeeds else "false") + ')\n', encoding="utf-8")
                    result = subprocess.run(
                        [sys.executable, str(root / "build.py"), "--parallel", "1", "--junit", str(root / "results.xml")],
                        env=environment, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", timeout=60)
                    self.assertEqual(result.returncode, 0 if succeeds else 8, result.stdout + result.stderr)
                    cache = (root / "build/release-ci/CMakeCache.txt").read_text(encoding="utf-8")
                    self.assertRegex(cache, r"(?m)^CMAKE_BUILD_TYPE:[A-Z]+=MinSizeRel$")
                    self.assertIn("LILTVIEW_VERSION_SUFFIX:UNINITIALIZED=-rc.1", cache)
                    self.assertTrue((root / "results.xml").is_file())


if __name__ == "__main__":
    unittest.main()
