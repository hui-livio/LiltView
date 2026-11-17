"""Embedded notices must remain complete and fit the VS 2022 literal limit."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "requires CMake")
class NoticeTests(unittest.TestCase):
    def test_long_utf8_licenses_are_lossless_bounded_literals(self):
        module = ROOT / "cmake/LiltViewNotices.cmake"
        listing = module.read_text(encoding="utf-8").split("set(LILTVIEW_NOTICES", 1)[0]
        files = re.findall(r'"([^"\n]+)"', listing)
        self.assertIn("LICENSE", files)
        self.assertIn("assets/fonts/LICENSE.HarmonyOS-Sans.txt", files)
        with tempfile.TemporaryDirectory(prefix="liltview notice source ") as directory:
            root = Path(directory)
            template = root / "app/resources/notices.hpp.in"
            template.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / "app/resources/notices.hpp.in", template)
            # Include a >64-KiB line, UTF-8, semicolons, quote/backslash bytes, and
            # the old raw-literal terminator. No source escaping may alter them.
            long_license = ('中文; \\ " )LV_LICENSES" ${literal} ' * 4096) + "\nEND OF LICENSE\n"
            expected = "LiltView - licenses and third-party notices\n"
            for name in files:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                text = long_license if name == "LICENSE" else "license text\n"
                if name == "assets/fonts/LICENSE.HarmonyOS-Sans.txt":
                    path.write_bytes(text.encode("utf-8") + b"\0" * 16)
                    text = "license text"  # Existing font NUL-padding normalization.
                else:
                    path.write_text(text, encoding="utf-8", newline="\n")
                expected += f"\n========== {name} ==========\n{text}\n"
            script = root / "generate.cmake"
            script.write_text(f'set(PROJECT_SOURCE_DIR "{root.as_posix()}")\n'
                              f'set(CMAKE_CURRENT_BINARY_DIR "{(root / "out").as_posix()}")\n'
                              f'include("{module.as_posix()}")\n', encoding="utf-8")
            subprocess.run(["cmake", "-P", str(script)], check=True, capture_output=True)
            header = (root / "out/generated/notices.hpp").read_text(encoding="ascii")
            literals = re.findall(r'"((?:\\x[0-9a-f]{2})+)"', header)
            self.assertGreater(len(literals), 1)
            self.assertTrue(all(len(literal) <= 4096 for literal in literals))
            decoded = bytes.fromhex("".join(literals).replace("\\x", ""))
            self.assertEqual(decoded, expected.encode("utf-8"))
            self.assertNotIn(b"\0", decoded)
            self.assertNotIn('R"LV_LICENSES(', header)


if __name__ == "__main__":
    unittest.main()
