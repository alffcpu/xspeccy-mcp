#!/usr/bin/env python3
"""Download the pinned Xpeccy source and build xspeccy-mcp."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from typing import Dict, Iterable


REPO_ROOT = Path(__file__).resolve().parent
VERSIONS_FILE = REPO_ROOT / "VERSIONS"

# A commit, not a tag. Tags can be moved and two of upstream's - `stable` and
# `minor` - are moving pointers by design, so an archive fetched by tag name is
# not a fixed thing. A commit SHA is.
XPECCY_COMMIT_URL = "https://github.com/samstyle/Xpeccy/archive/{commit}.tar.gz"
XPECCY_TAG_URL = "https://github.com/samstyle/Xpeccy/archive/refs/tags/{version}.tar.gz"


def read_versions() -> Dict[str, str]:
    """Parse the VERSIONS file, the one place any version is written down.

    cmake/version.cmake reads the same file with the same rules, so the build
    and this script cannot disagree about which Xpeccy is meant.
    """
    if not VERSIONS_FILE.is_file():
        raise SystemExit("VERSIONS is missing from " + str(REPO_ROOT))
    values: Dict[str, str] = {}
    for raw in VERSIONS_FILE.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            raise SystemExit("VERSIONS: cannot parse " + repr(raw))
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    for required in ("XSPECCY_MCP_VERSION", "XPECCY_VERSION", "XPECCY_COMMIT",
                     "XPECCY_LIBXPECCY_SHA256"):
        if not values.get(required):
            raise SystemExit("VERSIONS has no " + required)
    return values


VERSIONS = read_versions()
DEFAULT_XPECCY_VERSION = VERSIONS["XPECCY_VERSION"]


def libxpeccy_digest(source: Path) -> str:
    """Hash the sources we actually compile, ignoring how they reached the disk.

    Not a hash of the tarball. GitHub builds those on demand and has changed
    their compression before now, so a tarball hash can go stale while every
    line of source stays put.

    Line endings are normalised first. A tree checked out through git on Windows
    carries CRLF and would otherwise fail this check on every file while being
    character for character the same C.
    """
    root = source / "src" / "libxpeccy"
    # Sorted by the POSIX relative path as text, not by Path objects. Comparing
    # Path sorts by the platform's own separator, so "cpu/Z80/z80.c" orders
    # differently under a backslash than under a slash and the same sources would
    # hash differently on Windows and on Linux.
    files = sorted(
        (p.relative_to(root).as_posix(), p) for p in root.rglob("*") if p.is_file()
    )
    digest = hashlib.sha256()
    for relative, path in files:
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
        digest.update(b"\0")
    return digest.hexdigest()


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


def verify_xpeccy(source: Path, version: str) -> Path:
    """Check a tree this script fetched itself against the pinned hash.

    Refuses rather than warns: nobody chose this copy, the script downloaded it,
    and sources arriving from the network that are not the pinned ones is the one
    case worth stopping for. A tree named with --xpeccy-src is a deliberate
    choice and is only warned about.
    """
    if version != VERSIONS["XPECCY_VERSION"]:
        return source			# --xpeccy-version asked for something else
    want = VERSIONS["XPECCY_LIBXPECCY_SHA256"]
    got = libxpeccy_digest(source)
    if got != want:
        shutil.rmtree(str(source), ignore_errors=True)
        raise SystemExit(
            "The Xpeccy sources are not the pinned ones.\n"
            "  expected: " + want + "\n"
            "  got:      " + got + "\n"
            "Commit " + VERSIONS["XPECCY_COMMIT"] + " always gives the same files, so "
            "either VERSIONS is out of date, the copy under _deps was edited, or the "
            "download was tampered with. That copy has been removed; run again to "
            "fetch it afresh."
        )
    print("Xpeccy " + version + " verified (" + want[:16] + "...)", flush=True)
    return source


def pin_xpeccy(version: str, write: bool) -> int:
    """Work out what VERSIONS should say to pin `version`, and optionally say it.

    Moving to a new Xpeccy is a deliberate act, and the only awkward part of it
    is the source hash, which cannot be known without fetching the thing. Doing
    it by hand is how a wrong hash gets committed, so it is done here instead.

    Prints by default and writes only when asked, which is how tools/publish.py
    behaves as well: the irreversible step is never the one you get by accident.
    """
    api = "https://api.github.com/repos/samstyle/Xpeccy/git/ref/tags/" + version
    request = urllib.request.Request(api, headers={"User-Agent": "xspeccy-mcp-build"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            reference = json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            raise SystemExit("No Xpeccy tag called " + version)
        raise SystemExit("Could not look up tag " + version + ": " + str(error))
    commit = reference["object"]["sha"]
    # An annotated tag points at a tag object, which in turn points at the
    # commit; a lightweight one points straight at the commit.
    if reference["object"]["type"] == "tag":
        request = urllib.request.Request(
            "https://api.github.com/repos/samstyle/Xpeccy/git/tags/" + commit,
            headers={"User-Agent": "xspeccy-mcp-build"},
        )
        with urllib.request.urlopen(request, timeout=60) as response:
            commit = json.load(response)["object"]["sha"]

    with tempfile.TemporaryDirectory(prefix="xpeccy-pin-") as temporary:
        scratch = Path(temporary)
        archive_path = scratch / "xpeccy.tar.gz"
        download(XPECCY_COMMIT_URL.format(commit=commit), archive_path)
        extraction_dir = scratch / "tree"
        extraction_dir.mkdir()
        with tarfile.open(str(archive_path), "r:gz") as archive:
            safe_extract(archive, extraction_dir)
        digest = libxpeccy_digest(find_xpeccy_source(extraction_dir))

    updates = {
        "XPECCY_VERSION": version,
        "XPECCY_COMMIT": commit,
        "XPECCY_LIBXPECCY_SHA256": digest,
    }
    for key, value in updates.items():
        current = VERSIONS.get(key, "")
        mark = " (unchanged)" if current == value else ""
        print(key + " = " + value + mark)
        if current != value and current:
            print("    was: " + current)

    if not write:
        print("\nNothing written. Run again with --write to update VERSIONS.")
        return 0

    text = VERSIONS_FILE.read_text(encoding="utf-8")
    for key, value in updates.items():
        text = re.sub(
            r"(?m)^([ \t]*" + key + r"[ \t]*=[ \t]*).*$",
            lambda match, v=value: match.group(1) + v,
            text,
        )
    VERSIONS_FILE.write_text(text, encoding="utf-8")
    print("\nVERSIONS updated. Now rebuild and run the tests:")
    print("    python build.py --smoke")
    print("The expected values in tests/smoke.sh were measured against the old")
    print("release, so treat any failure as a real difference until shown otherwise.")
    return 0


def prepare_xpeccy(build_dir: Path, version: str) -> Path:
    dependencies = build_dir / "_deps"
    dependencies.mkdir(parents=True, exist_ok=True)
    source = dependencies / ("Xpeccy-" + version)
    if valid_xpeccy_source(source):
        # Checked on every build, not only after a download. The tree is cached
        # between builds, and this repository's standing rule is that the Xpeccy
        # sources are never edited - only compiled where they lie. An accidental
        # edit is exactly what this catches, and re-hashing 195 files is quick.
        return verify_xpeccy(source, version)

    archive_path = dependencies / ("Xpeccy-" + version + ".tar.gz")
    if not archive_path.exists():
        # By commit when this is the pinned version, which is the immutable
        # thing; by tag only when someone asked for a different version on the
        # command line, where no commit is known.
        if version == VERSIONS["XPECCY_VERSION"]:
            url = XPECCY_COMMIT_URL.format(commit=VERSIONS["XPECCY_COMMIT"])
        else:
            url = XPECCY_TAG_URL.format(version=version)
        download(url, archive_path)

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

    return verify_xpeccy(source, version)


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
        "--pin-xpeccy",
        metavar="VERSION",
        help="look up an Xpeccy release, hash its sources and report the VERSIONS "
        "lines that would pin it; writes nothing without --write",
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="with --pin-xpeccy, apply the change to VERSIONS",
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
    if args.pin_xpeccy is not None:
        # A maintenance action, not a build: it needs no compiler and must not
        # drag one in as a prerequisite.
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.pin_xpeccy):
            raise SystemExit("--pin-xpeccy must look like 0.6.20260804")
        return pin_xpeccy(args.pin_xpeccy, args.write)
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
        # Say so, but build anyway. Pointing this at a patched or a newer tree is
        # a deliberate act - the emulator's GUI needs patching to compile on
        # Windows at all - and the binary records which tree it came from.
        got = libxpeccy_digest(xpeccy_src)
        if got == VERSIONS["XPECCY_LIBXPECCY_SHA256"]:
            print("Xpeccy sources match the pinned " + VERSIONS["XPECCY_VERSION"],
                  flush=True)
        else:
            print(
                "warning: these Xpeccy sources are not the pinned "
                + VERSIONS["XPECCY_VERSION"] + "\n"
                "  expected: " + VERSIONS["XPECCY_LIBXPECCY_SHA256"] + "\n"
                "  got:      " + got + "\n"
                "  The build will go ahead; the test suite's expected values were\n"
                "  measured against the pinned release.",
                flush=True,
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
