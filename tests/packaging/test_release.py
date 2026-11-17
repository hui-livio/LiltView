"""Packaging contracts; no network or third-party Python packages required."""
import importlib.util
import io
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location("release", Path(__file__).resolve().parents[2] / "scripts/package_release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Test")
        self.git("config", "user.email", "test@example.invalid")
        for name, text in {"CMakeLists.txt": "project(LiltView VERSION 1.0.0)",
                           ".gitignore": "ignored/", ".github/workflows/test.yml": "ci",
                           "third_party/vendor/LICENSE": "vendor license",
                           "third_party/vendor/.gitignore": "build/", "main.c": "int main(void) {}",
                           "docs/中文说明.md": "完整源码与第三方依赖"}.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")

    def git(self, *args):
        return release.git(self.root, *args)

    def test_complete_deterministic_sources_without_git_metadata(self):
        a = Path(self.temp.name) / "a.tar.gz"
        b = Path(self.temp.name) / "b.tar.gz"
        release.write_source(a, self.root)
        release.write_source(b, self.root)
        self.assertEqual(a.read_bytes(), b.read_bytes())
        with tarfile.open(a) as archive:
            self.assertEqual(archive.extractfile("liltview-1.0.0/RELEASE_VERSION").read(), b"1.0.0\n")
            names = archive.getnames()
            self.assertIn("liltview-1.0.0/third_party/vendor/LICENSE", names)
            self.assertIn("liltview-1.0.0/main.c", names)
            self.assertFalse(any(release.excluded(Path(name)) for name in names))
            self.assertTrue(all(item.uid == 0 and item.gid == 0 for item in archive.getmembers()))

    def test_zip_matches_tar_snapshot_and_is_deterministic_with_utf8_names(self):
        source = Path(self.temp.name) / "source.tar.gz"
        first = Path(self.temp.name) / "first.zip"
        second = Path(self.temp.name) / "second.zip"
        release.write_source(source, self.root)
        release.write_source_zip(source, first)
        # Changing the checkout after tar creation must not change the ZIP snapshot.
        (self.root / "main.c").write_text("changed after snapshot", encoding="utf-8")
        release.write_source_zip(source, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with tarfile.open(source) as tar, zipfile.ZipFile(first) as archive:
            self.assertEqual(tar.getnames(), archive.namelist())
            for member in tar:
                info = archive.getinfo(member.name)
                self.assertEqual(tar.extractfile(member).read(), archive.read(info))
                self.assertEqual(info.external_attr >> 16, stat.S_IFREG | member.mode)
            chinese = archive.getinfo("liltview-1.0.0/docs/中文说明.md")
            self.assertTrue(chinese.flag_bits & 0x800)  # Explicit UTF-8 names.
            extracted = Path(self.temp.name) / "extracted"
            archive.extractall(extracted)
            self.assertEqual((extracted / chinese.filename).read_text(encoding="utf-8"),
                             "完整源码与第三方依赖")
            self.assertEqual(archive.read("liltview-1.0.0/RELEASE_VERSION"), b"1.0.0\n")
            self.assertFalse(any(release.excluded(Path(name)) for name in archive.namelist()))

    def test_zip_preserves_executable_symlink_and_clamps_dos_timestamps(self):
        source = Path(self.temp.name) / "metadata.tar.gz"
        output = Path(self.temp.name) / "metadata.zip"
        with tarfile.open(source, "w:gz") as archive:
            executable = tarfile.TarInfo("liltview-1.0.0/run.sh")
            executable.mode, executable.mtime, executable.size = 0o755, 0, 4
            archive.addfile(executable, io.BytesIO(b"test"))
            link = tarfile.TarInfo("liltview-1.0.0/link")
            link.type, link.linkname, link.mode = tarfile.SYMTYPE, "run.sh", 0o777
            link.mtime = 9999999999
            archive.addfile(link)
        release.write_source_zip(source, output)
        with zipfile.ZipFile(output) as archive:
            executable = archive.getinfo(executable.name)
            self.assertEqual(executable.external_attr >> 16, stat.S_IFREG | 0o755)
            self.assertEqual(executable.date_time, (1980, 1, 1, 0, 0, 0))
            link = archive.getinfo(link.name)
            self.assertEqual(link.external_attr >> 16, stat.S_IFLNK | 0o777)
            self.assertEqual(link.date_time, (2107, 12, 31, 23, 59, 58))
            self.assertEqual(archive.read(link), b"run.sh")

    def test_source_command_creates_both_archive_formats(self):
        script = self.root / "scripts/package_release.py"
        script.parent.mkdir()
        script.write_bytes(Path(release.__file__).read_bytes())
        self.git("add", ".")
        self.git("commit", "-qm", "packaging script")
        output = Path(self.temp.name) / "dist"
        env = dict(os.environ, LILTVIEW_RELEASE_VERSION="1.0.0-rc.1")
        subprocess.run([sys.executable, str(script), "source", "--output", str(output)],
                       env=env, check=True, capture_output=True)
        self.assertEqual(sorted(path.name for path in output.iterdir()),
                         ["liltview-1.0.0-rc.1-source.tar.gz", "liltview-1.0.0-rc.1-source.zip"])
        with zipfile.ZipFile(output / "liltview-1.0.0-rc.1-source.zip") as archive:
            self.assertEqual(archive.read("liltview-1.0.0-rc.1/RELEASE_VERSION"), b"1.0.0-rc.1\n")

    def test_workflow_publishes_both_sources_and_extracts_zip_on_windows(self):
        workflow = (release.ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("build/dist/*-source.tar.gz", workflow)
        self.assertIn("build/dist/*-source.zip", workflow)
        windows = workflow.split("\n  windows:", 1)[1].split("\n  macos:", 1)[0]
        self.assertIn('Expand-Archive -LiteralPath "incoming/liltview-${env:VERSION}-source.zip"', windows)
        self.assertNotIn("tar -xf", windows)
        self.assertIn("--notes-file ../source-notes.md", workflow)

    def test_recursive_dependency_sources_are_included_and_missing_checkout_rejected(self):
        child = Path(self.temp.name) / "child"
        parent = Path(self.temp.name) / "parent"
        for path in (child, parent):
            path.mkdir()
            for args in [("init", "-q"), ("config", "user.name", "Test"), ("config", "user.email", "test@example.invalid")]:
                release.git(path, *args)
            (path / "library.c").write_text("source", encoding="utf-8")
            release.git(path, "add", ".")
            release.git(path, "commit", "-qm", "library")
        release.git(parent, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(child), "nested")
        release.git(parent, "commit", "-qam", "nested dependency")
        self.git("-c", "protocol.file.allow=always", "submodule", "add", "-q", str(parent), "third_party/submodule")
        self.git("-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive")
        self.git("commit", "-qam", "pinned dependency")
        archive = Path(self.temp.name) / "submodules.tar.gz"
        release.write_source(archive, self.root)
        with tarfile.open(archive) as source:
            self.assertIn("liltview-1.0.0/third_party/submodule/nested/library.c", source.getnames())
            self.assertFalse(any(release.excluded(Path(name)) for name in source.getnames()))
        zipped = Path(self.temp.name) / "submodules.zip"
        release.write_source_zip(archive, zipped)
        with zipfile.ZipFile(zipped) as source:
            self.assertEqual(source.read("liltview-1.0.0/third_party/submodule/nested/library.c"), b"source")
            self.assertFalse(any(release.excluded(Path(name)) for name in source.namelist()))
        self.git("submodule", "deinit", "--force", "third_party/submodule")
        with self.assertRaisesRegex(ValueError, "Initialize all pinned"):
            release.write_source(archive, self.root)

    def test_dirty_or_missing_sources_are_rejected(self):
        (self.root / "main.c").write_text("dirty")
        with self.assertRaisesRegex(ValueError, "clean checkout"):
            release.write_source(Path(self.temp.name) / "a.tar.gz", self.root)
        (self.root / "main.c").unlink()
        with self.assertRaisesRegex(ValueError, "Missing tracked"):
            release.source_entries(self.root)

    def test_checksums_are_sorted_and_do_not_hash_themselves(self):
        output = Path(self.temp.name) / "dist"
        output.mkdir()
        (output / "b.exe").write_bytes(b"binary")
        (output / "a.tar.gz").write_bytes(b"source")
        release.checksums(output)
        expected = (output / "SHA256SUMS").read_text()
        release.checksums(output)
        self.assertEqual(expected, (output / "SHA256SUMS").read_text())
        self.assertEqual([line.split()[1] for line in expected.splitlines()], ["a.tar.gz", "b.exe"])

    def test_preview_and_stable_tags_cannot_change_base_version(self):
        self.assertEqual(release.version_for_ref("refs/heads/main", self.root), "1.0.0-rc.1")
        self.assertEqual(release.version_for_ref("refs/tags/v1.0.0-rc.2", self.root), "1.0.0-rc.2")
        self.assertEqual(release.version_for_ref("refs/tags/v1.0.0", self.root), "1.0.0")
        for tag in ["v1.0.1", "v1.0.0-rc.0", "v1.0.0-junk", "1.0.0"]:
            with self.assertRaises(ValueError):
                release.version_for_ref("refs/tags/" + tag, self.root)

    def test_arch_recipe_is_directly_usable_not_a_generated_template(self):
        recipe = (release.ROOT / "packaging/arch/PKGBUILD").read_text(encoding="utf-8")
        self.assertNotRegex(recipe, r"@[A-Z_]+@")
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", recipe)
        self.assertIn('-DLILTVIEW_BUILD_UNIT_TESTS="${_build_tests}"', recipe)
        self.assertIn('-DBUILD_TESTING="${_build_tests}"', recipe)
        self.assertIn("ctest --test-dir", recipe)
        self.assertIn("source=()", recipe)
        self.assertFalse((release.ROOT / "packaging/arch/PKGBUILD.in").exists())

    def test_git_administrative_paths_are_filtered_recursively(self):
        for path in [".git/config", "third_party/lib/.git", "third_party/lib/.gitattributes", ".github/workflows/a.yml"]:
            self.assertTrue(release.excluded(Path(path)))
        self.assertFalse(release.excluded(Path("third_party/lib/source.json")))


if __name__ == "__main__":
    unittest.main()
