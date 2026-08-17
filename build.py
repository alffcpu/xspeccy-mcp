#!/usr/bin/env python3
"""Download the pinned Xpeccy source and build xspeccy-mcp."""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from typing import Iterable


DEFAULT_XPECCY_VERSION = "0.6.20260804"
XPECCY_URL = (
    "https://github.com/samstyle/Xpeccy/archive/refs/tags/"
    "{version}.tar.gz"
)


def command_line(command: Iterable[object]) -> str:
    return " ".join(shlex.quote(str(part)) for part in command)


def run(command: Iterable[object], cwd: Path) -> None:
    command = [str(part) for part in command]
    print("+ " + command_line(command), flush=True)
    subprocess.run(command, cwd=str(cwd), check=True)


def valid_xpeccy_source(path: Path) -> bool:
    return (path / "src" / "libxpeccy" / "spectrum.h").is_file()


def find_xpeccy_source(root: Path) -> Path:
    matches = []
    for header in root.rglob("spectrum.h"):
        if header.parent.name == "libxpeccy" and header.parent.parent.name == "src":
            source = header.parent.parent.parent
            if source not in matches:
                matches.append(source)
    if len(matches) != 1:
        raise RuntimeError(
            "The Xpeccy archive did not contain exactly one src/libxpeccy tree"
        )
    return matches[0]


def safe_extract(archive: tarfile.TarFile, destination: Path) -> None:
    destination = destination.resolve()
    for member in archive.getmembers():
        if member.issym() or member.islnk():
            raise RuntimeError("Refusing to extract a symbolic link from the archive")
        target = (destination / member.name).resolve()
        if target != destination and destination not in target.parents:
            raise RuntimeError("Refusing to extract a path outside the build directory")
    archive.extractall(str(destination))


def download(url: str, destination: Path) -> None:
    print("Downloading " + url, flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "xspeccy-mcp-build"})
    with urllib.request.urlopen(request, timeout=120) as response:
        with destination.open("wb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)


def prepare_xpeccy(build_dir: Path, version: str) -> Path:
    dependencies = build_dir / "_deps"
    dependencies.mkdir(parents=True, exist_ok=True)
    source = dependencies / ("Xpeccy-" + version)
    if valid_xpeccy_source(source):
        return source

    archive_path = dependencies / ("Xpeccy-" + version + ".tar.gz")
    if not archive_path.exists():
        download(XPECCY_URL.format(version=version), archive_path)

    extraction_dir = Path(
        tempfile.mkdtemp(prefix="xpeccy-extract-", dir=str(dependencies))
    )
    try:
        with tarfile.open(str(archive_path), "r:gz") as archive:
            safe_extract(archive, extraction_dir)
        extracted_source = find_xpeccy_source(extraction_dir)
        if source.exists():
            shutil.rmtree(str(source))
        shutil.move(str(extracted_source), str(source))
    finally:
        shutil.rmtree(str(extraction_dir), ignore_errors=True)
    return source


def resolve_path(root: Path, value: Path) -> Path:
    return value.resolve() if value.is_absolute() else (root / value).resolve()


def find_executable(build_dir: Path) -> Path:
    filename = "xspeccy-mcp.exe" if os.name == "nt" else "xspeccy-mcp"
    candidates = [
        build_dir / filename,
        build_dir / "Release" / filename,
        build_dir / "RelWithDebInfo" / filename,
        build_dir / "Debug" / filename,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download Xpeccy and build the xspeccy MCP server"
    )
    parser.add_argument(
        "--xpeccy-version",
        default=DEFAULT_XPECCY_VERSION,
        help="Xpeccy release tag (default: %(default)s)",
    )
    parser.add_argument(
        "--xpeccy-src",
        type=Path,
        help="use an existing Xpeccy source tree instead of downloading it",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="build directory (default: %(default)s)",
    )
    parser.add_argument(
        "--generator",
        help="CMake generator, for example Ninja or MinGW Makefiles",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=max(1, os.cpu_count() or 1),
        help="parallel build jobs (default: number of CPUs)",
    )
    parser.add_argument(
        "--cmake",
        default="cmake",
        help="CMake executable (default: %(default)s)",
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="run tests/smoke.sh against the binary once it is built",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.jobs < 1:
        raise SystemExit("--jobs must be at least 1")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.xpeccy_version):
        raise SystemExit("--xpeccy-version must look like 0.6.20260804")
    if shutil.which(args.cmake) is None and not Path(args.cmake).is_file():
        raise SystemExit("CMake was not found; install CMake 3.16 or newer")

    root = Path(__file__).resolve().parent
    build_dir = resolve_path(root, args.build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    if args.xpeccy_src is None:
        xpeccy_src = prepare_xpeccy(build_dir, args.xpeccy_version)
    else:
        xpeccy_src = resolve_path(root, args.xpeccy_src)
        if not valid_xpeccy_source(xpeccy_src):
            raise SystemExit(
                "--xpeccy-src must contain src/libxpeccy/spectrum.h: "
                + str(xpeccy_src)
            )

    configure = [args.cmake]
    if args.generator:
        configure.extend(["-G", args.generator])
    configure.extend(
        [
            "-S",
            root,
            "-B",
            build_dir,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DXPECCY_SRC=" + str(xpeccy_src),
        ]
    )
    run(configure, root)
    run(
        [args.cmake, "--build", build_dir, "--config", "Release", "--parallel", args.jobs],
        root,
    )

    executable = find_executable(build_dir)
    print("Built " + str(executable), flush=True)

    if args.smoke:
        # The suite is a bash script on every platform, including Windows, where
        # Git Bash and the MSYS2 shell both provide one. It takes the binary as
        # its argument, so the build directory does not have to be the default.
        smoke = root / "tests" / "smoke.sh"
        if not smoke.is_file():
            raise SystemExit("tests/smoke.sh is missing")
        bash = shutil.which("bash")
        if bash is None:
            raise SystemExit(
                "--smoke needs bash on PATH (Git Bash or MSYS2 on Windows)"
            )
        run([bash, smoke, executable], root)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
