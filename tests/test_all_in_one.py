"""Release archive construction and executable-permission tests."""

import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile

from scripts.package_all_in_one import (
    LINUX_ARCHES, WINDOWS_ARCHES, build_package, required_files,
)


class AllInOneTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.assets = self.base / "assets"
        self.assets.mkdir()
        self.readme = self.base / "README.md"
        self.linux_readme = self.base / "linux" / "README.md"
        self.linux_readme.parent.mkdir()
        self.readme.write_text("Windows first. USBGuard 安全保护\n", encoding="utf-8")
        self.linux_readme.write_text("Linux / Wayland / X11\n", encoding="utf-8")
        for file in required_files(self.assets, self.readme, self.linux_readme).values():
            if file in (self.readme, self.linux_readme):
                continue
            file.write_bytes(b"test release artifact: " + file.name.encode())

    def test_archive_layout_and_file_contents(self):
        result = self.base / "USBGuard-All-In-One.zip"
        build_package(self.assets, result, self.readme, self.linux_readme)
        expected = required_files(self.assets, self.readme, self.linux_readme)
        with ZipFile(result) as archive:
            self.assertEqual(set(archive.namelist()), set(expected))
            self.assertIsNone(archive.testzip())
            for name, path in expected.items():
                self.assertEqual(archive.read(name), path.read_bytes())
                mode = (archive.getinfo(name).external_attr >> 16) & 0o777
                if name.startswith("Linux/") and not name.endswith(".md"):
                    self.assertEqual(mode, 0o755)
                else:
                    self.assertEqual(mode, 0o644)
            self.assertIn("Windows/x86/USBGuardMini.exe", expected)
            self.assertIn("Windows/ARM64/USBGuardUI.exe", expected)
            self.assertIn("Linux/aarch64/USBGuardLinuxCLI", expected)
        self.assertEqual(len(WINDOWS_ARCHES), 3)
        self.assertEqual(len(LINUX_ARCHES), 2)

    def test_missing_binary_causes_failure(self):
        (self.assets / "USBGuardMini-x64.exe").unlink()
        with self.assertRaisesRegex(FileNotFoundError, "USBGuardMini-x64.exe"):
            build_package(self.assets, self.base / "invalid.zip",
                          self.readme, self.linux_readme)
        self.assertFalse((self.base / "invalid.zip").exists())

    def test_repeated_build_is_reproducible(self):
        first = self.base / "first.zip"
        second = self.base / "second.zip"
        build_package(self.assets, first, self.readme, self.linux_readme)
        build_package(self.assets, second, self.readme, self.linux_readme)
        self.assertEqual(first.read_bytes(), second.read_bytes())


if __name__ == "__main__":
    unittest.main()
