#!/usr/bin/env python3
"""Create the combined Windows/Linux release package without extra dependencies."""

from __future__ import annotations

import argparse
from pathlib import Path
import stat
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


WINDOWS_ARCHES = ("x86", "x64", "ARM64")
LINUX_ARCHES = ("x86_64", "aarch64")


def required_files(assets: Path, readme: Path, linux_readme: Path) -> dict[str, Path]:
    files: dict[str, Path] = {"README.md": readme, "Linux/README.md": linux_readme}
    for arch in WINDOWS_ARCHES:
        for executable in ("USBGuardUI", "USBGuardMini"):
            files[f"Windows/{arch}/{executable}.exe"] = assets / f"{executable}-{arch}.exe"
    for arch in LINUX_ARCHES:
        for executable in ("USBGuardLinux", "USBGuardLinuxCLI"):
            files[f"Linux/{arch}/{executable}"] = assets / f"{executable}-{arch}"
    return files


def build_package(assets: Path, output: Path, readme: Path, linux_readme: Path) -> None:
    files = required_files(assets, readme, linux_readme)
    absent = [str(source) for source in files.values() if not source.is_file()]
    if absent:
        raise FileNotFoundError("Missing files for All-In-One archive: " + ", ".join(absent))

    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", compression=ZIP_DEFLATED, compresslevel=9) as archive:
        for relative_path, source in sorted(files.items()):
            entry = ZipInfo(relative_path, date_time=(2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = (
                (stat.S_IFREG | (0o755 if relative_path.startswith("Linux/") and
                                 not relative_path.endswith(".md") else 0o644)) << 16
            )
            archive.writestr(entry, source.read_bytes(), compress_type=ZIP_DEFLATED,
                             compresslevel=9)

    with ZipFile(output) as archive:
        if set(archive.namelist()) != set(files):
            raise RuntimeError("All-In-One archive does not contain the expected files")
        if archive.testzip() is not None:
            raise RuntimeError("All-In-One archive failed integrity verification")
        for name, source in files.items():
            if archive.read(name) != source.read_bytes():
                raise RuntimeError(f"All-In-One archive content mismatch: {name}")
            info = archive.getinfo(name)
            if name.startswith("Linux/") and not name.endswith(".md"):
                if (info.external_attr >> 16) & 0o111 != 0o111:
                    raise RuntimeError(f"Linux executable permissions missing: {name}")

    print(f"Created {output} containing {len(files)} files "
          "(6 Windows EXEs, 4 Linux executables and 2 README files)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path, help="Directory containing Release binaries")
    parser.add_argument("output", type=Path, help="Combined ZIP output file")
    parser.add_argument("readme", type=Path, help="Main README")
    parser.add_argument("linux_readme", type=Path, help="Linux README")
    args = parser.parse_args()
    build_package(args.assets, args.output, args.readme, args.linux_readme)


if __name__ == "__main__":
    main()
