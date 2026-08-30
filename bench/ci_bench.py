#!/usr/bin/env python3
"""Compile and run the canvas_ity microbench with repeats, for CI.

Each process takes the best of --trials inner iterations (the harness already
does that).  This driver runs that process --repeats times after one warmup
and reports min / median / max and relative spread so GitHub-hosted-runner
noise is visible.

  python3 bench/ci_bench.py --repeats 5 --trials 5 --out bench.json
  python3 bench/ci_bench.py --summarize artifacts/
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

LINE = re.compile(r"^(\S+(?: \S+)*)\s+([0-9.]+)ms\s*$")


def repo_root() -> Path:
    here = Path(__file__).resolve().parent
    return here.parent


def median(values: list[float]) -> float:
    ordered = sorted(values)
    n = len(ordered)
    if n == 0:
        return float("nan")
    mid = n // 2
    if n % 2:
        return ordered[mid]
    return 0.5 * (ordered[mid - 1] + ordered[mid])


def capture(cmd: list[str], cwd: Path | None = None, check: bool = True) -> str:
    result = subprocess.run(
        cmd,
        cwd=cwd,
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result.stdout


def compiler_banner(compiler: str) -> str:
    try:
        if compiler == "cl":
            out = capture([compiler], check=False)
        else:
            out = capture([compiler, "--version"])
    except FileNotFoundError:
        return compiler
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    return lines[0] if lines else compiler


def compile_bench(root: Path, binary: Path) -> str:
    src = root / "bench" / "microbench.cpp"
    include = root / "src"
    binary.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        compiler = "cl"
        # /Fe must be one token with the output path.
        cmd = [
            compiler,
            "/nologo",
            "/O2",
            "/EHsc",
            "/std:c++14",
            "/I",
            str(include),
            "/Fe" + str(binary),
            str(src),
        ]
        capture(cmd, cwd=binary.parent)
        banner = compiler_banner(compiler)
    else:
        compiler = "c++"
        cmd = [
            compiler,
            "-O2",
            "-std=c++11",
            "-fno-exceptions",
            "-I",
            str(include),
            "-o",
            str(binary),
            str(src),
        ]
        capture(cmd)
        banner = compiler_banner(compiler)
    return banner


def run_prefix() -> list[str]:
    if sys.platform.startswith("linux") and shutil.which("taskset"):
        return ["taskset", "-c", "0"]
    return []


def parse_times(text: str) -> dict[str, float]:
    times: dict[str, float] = {}
    for line in text.splitlines():
        match = LINE.match(line)
        if match:
            times[match.group(1)] = float(match.group(2))
    if not times:
        raise RuntimeError("no workload times parsed:\n" + text[-800:])
    return times


def run_once(binary: Path, trials: int) -> dict[str, float]:
    cmd = run_prefix() + [str(binary), str(trials)]
    return parse_times(capture(cmd))


def summarize_workload(samples: list[float]) -> dict[str, float]:
    lo = min(samples)
    hi = max(samples)
    mid = median(samples)
    spread = (hi - lo) / mid if mid else float("inf")
    return {
        "min_ms": round(lo, 3),
        "median_ms": round(mid, 3),
        "max_ms": round(hi, 3),
        "spread": round(spread, 4),
        "n": len(samples),
    }


def markdown_table(report: dict) -> str:
    rows = [
        f"# Microbench ({report['os']} {report['arch']})",
        "",
        f"- compiler: `{report['compiler']}`",
        f"- repeats: {report['repeats']} processes after 1 warmup, "
        f"best-of-{report['trials']} trials each",
        "",
        "| workload | min ms | median ms | max ms | spread |",
        "|---|---:|---:|---:|---:|",
    ]
    for name, stats in report["workloads"].items():
        rows.append(
            "| {name} | {min_ms:.3f} | {median_ms:.3f} | {max_ms:.3f} | {spread:.1%} |".format(
                name=name, **stats
            )
        )
    return "\n".join(rows) + "\n"


def append_summary(text: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        print(text)
        return
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(text)
        if not text.endswith("\n"):
            handle.write("\n")
    print(text)


def bench(args: argparse.Namespace) -> int:
    root = repo_root()
    out = Path(args.out)
    binary = Path(args.binary) if args.binary else (
        root / "bench" / "build" / ("microbench.exe" if os.name == "nt" else "microbench")
    )
    compiler = compile_bench(root, binary)
    print("warmup", flush=True)
    run_once(binary, args.trials)
    collected: dict[str, list[float]] = {}
    order: list[str] = []
    for index in range(args.repeats):
        print(f"repeat {index + 1}/{args.repeats}", flush=True)
        times = run_once(binary, args.trials)
        if not order:
            order = list(times.keys())
        for name in order:
            collected.setdefault(name, []).append(times[name])
    workloads = {name: summarize_workload(collected[name]) for name in order}
    report = {
        "os": platform.system(),
        "arch": platform.machine(),
        "compiler": compiler,
        "repeats": args.repeats,
        "trials": args.trials,
        "workloads": workloads,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    append_summary(markdown_table(report))
    return 0


def load_reports(directory: Path) -> list[dict]:
    reports = []
    for path in sorted(directory.rglob("bench.json")):
        reports.append(json.loads(path.read_text(encoding="utf-8")))
    if not reports:
        raise SystemExit(f"no bench.json under {directory}")
    return reports


def label(report: dict) -> str:
    os_name = report["os"]
    arch = report["arch"]
    if os_name == "Linux" and arch in ("aarch64", "arm64"):
        return "linux-arm"
    if os_name == "Linux":
        return "linux-x64"
    if os_name == "Darwin":
        return "macos"
    if os_name == "Windows":
        return "windows"
    return f"{os_name}-{arch}"


def summarize(args: argparse.Namespace) -> int:
    reports = load_reports(Path(args.summarize))
    labels = [label(r) for r in reports]
    names: list[str] = []
    seen = set()
    for report in reports:
        for name in report["workloads"]:
            if name not in seen:
                seen.add(name)
                names.append(name)
    header = "| workload | " + " | ".join(labels) + " |"
    sep = "|---|" + "|".join(["---:" for _ in labels]) + "|"
    lines = [
        "# Cross-platform medians (ms)",
        "",
        "Each cell is the median of "
        f"{reports[0]['repeats']} process runs "
        f"(best-of-{reports[0]['trials']} trials each) "
        "on a GitHub-hosted runner. Spread is in each job's own table; "
        "do not treat sub-10% cross-run differences as real.",
        "",
        header,
        sep,
    ]
    for name in names:
        cells = [name]
        for report in reports:
            stats = report["workloads"].get(name)
            cells.append(f"{stats['median_ms']:.3f}" if stats else "")
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    lines.append("## Runner / compiler")
    lines.append("")
    lines.append("| platform | arch | compiler |")
    lines.append("|---|---|---|")
    for report, tag in zip(reports, labels):
        lines.append(
            f"| {tag} | {report['arch']} | `{report['compiler']}` |"
        )
    lines.append("")
    append_summary("\n".join(lines) + "\n")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--out", default="bench.json")
    parser.add_argument("--binary", default="")
    parser.add_argument(
        "--summarize",
        default="",
        help="Directory of downloaded bench.json artifacts; write a combined table.",
    )
    args = parser.parse_args()
    if args.summarize:
        return summarize(args)
    return bench(args)


if __name__ == "__main__":
    sys.exit(main())
