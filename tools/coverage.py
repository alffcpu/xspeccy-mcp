#!/usr/bin/env python3
"""Measure how much of src/ the test suites actually execute.

Both suites count. The unit tests cover the pure logic underneath the server and
the end-to-end suite in tests/smoke.sh covers the wiring, and neither number
alone says what fraction of the code has ever been run. So this builds one tree
with instrumentation, runs both, and merges the result.

It is a separate build tree on purpose. Coverage instrumentation changes the
code generation and drags a profile file into every run, so a binary built this
way is not the binary anyone should be serving from; keeping it under a
different directory means `python3 build.py` never has to know this exists.

Toolchains:
  clang   llvm-profdata + llvm-cov, which report lines, regions and branches
  gcc     gcov's JSON output, which reports lines and branches

Lines are the headline because that is the number people mean by "coverage", but
regions and branches are printed as well: a file can run every line and still
never take an `if` the other way, and that difference is the interesting part.
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent

# The floor the suite is expected to hold. Not an aspiration: below this the
# script exits non-zero, so a change that guts the tests fails rather than
# quietly lowering the number nobody looks at.
#
# Set below what the suite actually reaches - 91% on the reference build - and
# deliberately so. gcov and llvm-cov do not count the same lines, and a floor
# set at the measurement would fail on the other toolchain for a reason that has
# nothing to do with the tests. The gap is the tolerance, not slack to spend.
COVERAGE_FLOOR = 85.0


def run(command: List[object], cwd: Path = REPO_ROOT, env=None) -> None:
    printable = " ".join(str(c) for c in command)
    print("+ " + printable, flush=True)
    subprocess.run([str(c) for c in command], cwd=str(cwd), env=env, check=True)


def sources() -> List[Path]:
    """The files we are measuring: our own, not the emulator core or json.hpp.

    src/tools/ counts. It is the larger half of the code and the half a caller
    actually reaches, and leaving it out would make the number look better for
    the worst possible reason.
    """
    return (sorted((REPO_ROOT / "src").glob("*.cpp"))
            + sorted((REPO_ROOT / "src" / "tools").glob("*.cpp")))


def compiler_family() -> str:
    """clang or gcc, asked of the compiler cmake will pick up.

    Guessing from the platform would be wrong on both ends: Linux boxes carry
    clang and macOS can be pointed at a gcc through CXX.
    """
    compiler = os.environ.get("CXX") or "c++"
    try:
        out = subprocess.run([compiler, "--version"], capture_output=True,
                     text=True, check=False).stdout
    except OSError:
        raise SystemExit("no C++ compiler found; set CXX")
    return "clang" if "clang" in out.lower() else "gcc"


def llvm_tool(name: str) -> str:
    """llvm-profdata/llvm-cov, which on macOS live behind xcrun."""
    found = shutil.which(name)
    if found:
        return found
    if sys.platform == "darwin" and shutil.which("xcrun"):
        out = subprocess.run(["xcrun", "--find", name], capture_output=True,
                     text=True, check=False)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    raise SystemExit(
        name + " was not found. It ships with clang; on macOS it comes with the\n"
        "Command Line Tools, on Debian and Ubuntu with the llvm package.")


# ---------------------------------------------------------------- building

def configure_and_build(build_dir: Path, family: str, cmake: str, jobs: int,
                        xpeccy_src: Path) -> None:
    if family == "clang":
        flags = "-fprofile-instr-generate -fcoverage-mapping"
        link = "-fprofile-instr-generate"
    else:
        flags = "--coverage -fprofile-update=atomic"
        link = "--coverage"
    # Debug, because an optimiser that inlines a function away makes the line
    # it came from unattributable, and the number then depends on -O rather
    # than on the tests.
    run([cmake, "-S", REPO_ROOT, "-B", build_dir,
         "-DCMAKE_BUILD_TYPE=Debug",
         "-DXPECCY_SRC=" + str(xpeccy_src),
         "-DCMAKE_C_FLAGS=" + flags,
         "-DCMAKE_CXX_FLAGS=" + flags,
         "-DCMAKE_EXE_LINKER_FLAGS=" + link])
    run([cmake, "--build", build_dir, "--target", "xspeccy-mcp", "xspeccy-unit",
         "--parallel", jobs])


def binaries(build_dir: Path) -> Tuple[Path, Path]:
    server = build_dir / "xspeccy-mcp"
    unit = build_dir / "xspeccy-unit"
    if not server.is_file():
        server = server.with_suffix(".exe")   # Windows
    if not unit.is_file():
        unit = unit.with_suffix(".exe")
    if not server.is_file():
        raise SystemExit("no instrumented server at " + str(server))
    if not unit.is_file():
        raise SystemExit("no instrumented unit binary at " + str(unit))
    return server, unit


def run_suites(build_dir: Path, server: Path, unit: Path, family: str,
               skip_smoke: bool) -> Path:
    """Runs both suites and returns the directory holding the raw profiles."""
    profiles = build_dir / "profiles"
    if profiles.is_dir():
        shutil.rmtree(profiles)
    profiles.mkdir(parents=True)

    env = dict(os.environ)
    if family == "clang":
        # %p keeps the two runs from writing over each other
        env["LLVM_PROFILE_FILE"] = str(profiles / "%p-%m.profraw")

    run([unit], env=env)

    if skip_smoke:
        print("smoke suite skipped; the number below covers the unit tests only",
              flush=True)
        return profiles

    bash = shutil.which("bash")
    if bash is None:
        raise SystemExit("the end-to-end suite needs bash on PATH")
    run([bash, REPO_ROOT / "tests" / "smoke.sh", server], env=env)
    return profiles


# --------------------------------------------------------------- reporting

class Counts:
    """covered/total for one measure, so the four look the same to the printer."""

    def __init__(self) -> None:
        self.covered = 0
        self.total = 0

    def add(self, covered: int, total: int) -> None:
        self.covered += covered
        self.total += total

    def percent(self) -> float:
        return 100.0 * self.covered / self.total if self.total else 100.0


class FileReport:
    def __init__(self, name: str) -> None:
        self.name = name
        self.lines = Counts()
        self.regions = Counts()
        self.branches = Counts()
        self.functions = Counts()


def report_clang(server: Path, unit: Path, profiles: Path) -> List[FileReport]:
    raws = sorted(profiles.glob("*.profraw"))
    if not raws:
        raise SystemExit("no .profraw files were written; the build was not instrumented")
    merged = profiles / "merged.profdata"
    run([llvm_tool("llvm-profdata"), "merge", "-sparse", *raws, "-o", merged])

    out = subprocess.run(
        [llvm_tool("llvm-cov"), "export", str(server), "-object", str(unit),
         "-instr-profile=" + str(merged), *[str(s) for s in sources()]],
        capture_output=True, text=True, check=True).stdout
    data = json.loads(out)

    reports = []
    for entry in data["data"][0]["files"]:
        name = os.path.relpath(entry["filename"], REPO_ROOT)
        summary = entry["summary"]
        report = FileReport(name)
        report.lines.add(summary["lines"]["covered"], summary["lines"]["count"])
        report.regions.add(summary["regions"]["covered"], summary["regions"]["count"])
        report.branches.add(summary["branches"]["covered"], summary["branches"]["count"])
        report.functions.add(summary["functions"]["covered"], summary["functions"]["count"])
        reports.append(report)
    return reports


def report_gcc(build_dir: Path) -> List[FileReport]:
    """gcov's JSON output, one .gcda at a time.

    A source compiled into both binaries has two .gcda files, so the counts are
    accumulated per source name rather than per object: a line run only by the
    unit tests and a line run only by the server both count as covered.
    """
    gcov = shutil.which("gcov") or "gcov"
    wanted = {s.name for s in sources()}
    by_name: Dict[str, Dict[int, Tuple[int, int, int]]] = {}

    for gcda in sorted(build_dir.rglob("*.gcda")):
        result = subprocess.run(
            [gcov, "--json-format", "--stdout", str(gcda)],
            capture_output=True, check=False, cwd=str(gcda.parent))
        if result.returncode != 0 or not result.stdout:
            continue
        payload = result.stdout
        if payload[:2] == b"\x1f\x8b":
            payload = gzip.decompress(payload)
        for chunk in payload.decode(errors="replace").splitlines():
            if not chunk.strip():
                continue
            try:
                doc = json.loads(chunk)
            except json.JSONDecodeError:
                continue
            for entry in doc.get("files", []):
                name = os.path.basename(entry["file"])
                if name not in wanted:
                    continue
                lines = by_name.setdefault(name, {})
                for line in entry.get("lines", []):
                    number = line["line_number"]
                    count = line.get("count", 0)
                    branches = line.get("branches", [])
                    taken = sum(1 for b in branches if b.get("count", 0) > 0)
                    previous = lines.get(number, (0, 0, 0))
                    lines[number] = (previous[0] + count,
                                     max(previous[1], len(branches)),
                                     max(previous[2], taken))

    reports = []
    for name in sorted(by_name):
        report = FileReport(os.path.join("src", name))
        for _, (count, branches, taken) in by_name[name].items():
            report.lines.add(1 if count > 0 else 0, 1)
            report.branches.add(taken, branches)
        reports.append(report)
    return reports


def print_table(reports: List[FileReport], family: str) -> Counts:
    totals = FileReport("TOTAL")
    for report in reports:
        for measure in ("lines", "regions", "branches", "functions"):
            counts = getattr(report, measure)
            getattr(totals, measure).add(counts.covered, counts.total)

    show_regions = family == "clang"
    header = "%-24s %8s %8s" % ("file", "lines", "branch")
    if show_regions:
        header += " %8s %8s" % ("region", "funcs")
    print("")
    print(header)
    print("-" * len(header))

    def row(report: FileReport) -> str:
        text = "%-24s %7.1f%% %7.1f%%" % (report.name.replace("src/", ""),
                                          report.lines.percent(),
                                          report.branches.percent())
        if show_regions:
            text += " %7.1f%% %7.1f%%" % (report.regions.percent(),
                                          report.functions.percent())
        return text

    # Weakest first: the point of the table is what to test next.
    for report in sorted(reports, key=lambda r: r.lines.percent()):
        print(row(report))
    print("-" * len(header))
    print(row(totals) + "   (%d/%d lines)" % (totals.lines.covered, totals.lines.total))
    return totals.lines


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build with coverage instrumentation, run both test suites "
                    "and report how much of src/ they execute")
    parser.add_argument("--build-dir", type=Path, default=Path("build-coverage"),
                        help="build directory (default: %(default)s)")
    parser.add_argument("--xpeccy-src", type=Path,
                        help="Xpeccy source tree; by default the one under build/")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--min", type=float, default=COVERAGE_FLOOR,
                        metavar="PERCENT",
                        help="fail below this line coverage (default: %(default)s)")
    parser.add_argument("--no-smoke", action="store_true",
                        help="unit tests only, which measures the pure logic alone")
    parser.add_argument("--skip-build", action="store_true",
                        help="reuse the instrumented tree from a previous run")
    args = parser.parse_args()

    build_dir = args.build_dir
    if not build_dir.is_absolute():
        build_dir = REPO_ROOT / build_dir

    xpeccy_src = args.xpeccy_src
    if xpeccy_src is None:
        found = sorted((REPO_ROOT / "build" / "_deps").glob("Xpeccy-*"))
        found = [p for p in found if (p / "src" / "libxpeccy" / "spectrum.h").is_file()]
        if not found:
            raise SystemExit(
                "no Xpeccy source tree found. Run `python3 build.py` first, or "
                "pass --xpeccy-src.")
        xpeccy_src = found[-1]

    family = compiler_family()
    if not args.skip_build:
        configure_and_build(build_dir, family, args.cmake, args.jobs, xpeccy_src)

    server, unit = binaries(build_dir)
    profiles = run_suites(build_dir, server, unit, family, args.no_smoke)

    reports = (report_clang(server, unit, profiles) if family == "clang"
               else report_gcc(build_dir))
    if not reports:
        raise SystemExit("no coverage data was produced")
    lines = print_table(reports, family)

    print("")
    if lines.percent() + 0.05 < args.min:
        print("FAIL: %.2f%% of lines, below the %.1f%% floor"
              % (lines.percent(), args.min))
        return 1
    print("OK: %.2f%% of lines, floor is %.1f%%" % (lines.percent(), args.min))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
