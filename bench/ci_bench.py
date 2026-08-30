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
import math
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


def compile_bench(root: Path, binary: Path, include: Path) -> str:
    src = (root / "bench" / "microbench.cpp").resolve()
    binary = binary.resolve()
    include = include.resolve()
    binary.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        compiler = "cl"
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
        return compiler_banner(compiler)
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
    return compiler_banner(compiler)


def named_order(times: dict[str, float]) -> list[str]:
    return [name for name in times if name != "geo mean"]


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


def geo_mean(values: list[float]) -> float:
    if not values or any(v <= 0 for v in values):
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def pct_delta(base: float, ours: float) -> float:
    if not base:
        return float("nan")
    return 100.0 * (ours - base) / base


def markdown_table(report: dict) -> str:
    if report.get("mode") == "ab":
        return markdown_ab(report)
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


def markdown_ab(report: dict) -> str:
    rows = [
        f"# A/B microbench ({report['os']} {report['arch']})",
        "",
        f"- compiler: `{report['compiler']}`",
        f"- base: `{report.get('base_label', 'base')}`  ours: HEAD",
        f"- {report['repeats']} interleaved process pairs after warmup, "
        f"best-of-{report['trials']} trials each",
        f"- geo-mean delta: **{report['geo_delta_pct']:+.1f}%** "
        f"(base {report['geo_a']:.3f} ms -> ours {report['geo_b']:.3f} ms)",
        "",
        "| workload | base median | ours median | delta | spread base | spread ours |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for name, stats in report["workloads"].items():
        rows.append(
            "| {name} | {median_a:.3f} | {median_b:.3f} | {delta_pct:+.1f}% | "
            "{spread_a:.1%} | {spread_b:.1%} |".format(name=name, **stats)
        )
    return "\n".join(rows) + "\n"


def append_summary(text: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(text)
            if not text.endswith("\n"):
                handle.write("\n")
    try:
        print(text, flush=True)
    except UnicodeEncodeError:
        sys.stdout.buffer.write((text + "\n").encode(sys.stdout.encoding or "utf-8", errors="replace"))


def bench(args: argparse.Namespace) -> int:
    root = repo_root()
    out = Path(args.out)
    binary = Path(args.binary) if args.binary else (
        root / "bench" / "build" / ("microbench.exe" if os.name == "nt" else "microbench")
    )
    compiler = compile_bench(root, binary, root / "src")
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
        "mode": "single",
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


def ab_bench(args: argparse.Namespace) -> int:
    root = repo_root()
    out = Path(args.out)
    ext = ".exe" if os.name == "nt" else ""
    build = root / "bench" / "build"
    bin_a = build / f"micro_a{ext}"
    bin_b = build / f"micro_b{ext}"
    base_include = Path(args.base_include)
    compiler = compile_bench(root, bin_a, base_include)
    compile_bench(root, bin_b, root / "src")
    print("warmup A", flush=True)
    run_once(bin_a, args.trials)
    print("warmup B", flush=True)
    run_once(bin_b, args.trials)
    collected_a: dict[str, list[float]] = {}
    collected_b: dict[str, list[float]] = {}
    order: list[str] = []
    for index in range(args.repeats):
        print(f"repeat {index + 1}/{args.repeats} A", flush=True)
        times_a = run_once(bin_a, args.trials)
        print(f"repeat {index + 1}/{args.repeats} B", flush=True)
        times_b = run_once(bin_b, args.trials)
        if not order:
            order = named_order(times_a)
        for name in order:
            collected_a.setdefault(name, []).append(times_a[name])
            collected_b.setdefault(name, []).append(times_b[name])
    workloads = {}
    med_a = []
    med_b = []
    for name in order:
        sa = summarize_workload(collected_a[name])
        sb = summarize_workload(collected_b[name])
        delta = pct_delta(sa["median_ms"], sb["median_ms"])
        workloads[name] = {
            "median_a": sa["median_ms"],
            "median_b": sb["median_ms"],
            "delta_pct": round(delta, 2),
            "spread_a": sa["spread"],
            "spread_b": sb["spread"],
            "min_a": sa["min_ms"],
            "max_a": sa["max_ms"],
            "min_b": sb["min_ms"],
            "max_b": sb["max_ms"],
        }
        med_a.append(sa["median_ms"])
        med_b.append(sb["median_ms"])
    geo_a = geo_mean(med_a)
    geo_b = geo_mean(med_b)
    report = {
        "mode": "ab",
        "os": platform.system(),
        "arch": platform.machine(),
        "compiler": compiler,
        "repeats": args.repeats,
        "trials": args.trials,
        "base_label": args.base_label,
        "geo_a": round(geo_a, 3),
        "geo_b": round(geo_b, 3),
        "geo_delta_pct": round(pct_delta(geo_a, geo_b), 2),
        "workloads": workloads,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    append_summary(markdown_ab(report))
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
    if reports[0].get("mode") == "ab":
        return summarize_ab(reports, labels)
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


def summarize_ab(reports: list[dict], labels: list[str]) -> int:
    names: list[str] = []
    seen = set()
    for report in reports:
        for name in report["workloads"]:
            if name not in seen:
                seen.add(name)
                names.append(name)
    header = "| workload | " + " | ".join(f"{tag} d%" for tag in labels) + " |"
    sep = "|---|" + "|".join(["---:" for _ in labels]) + "|"
    lines = [
        "# Cross-platform A/B (ours vs base, percent; negative is faster)",
        "",
        header,
        sep,
    ]
    for name in names:
        cells = [name]
        for report in reports:
            stats = report["workloads"].get(name)
            cells.append(f"{stats['delta_pct']:+.1f}%" if stats else "")
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    lines.append("| platform | geo base ms | geo ours ms | geo d% |")
    lines.append("|---|---:|---:|---:|")
    for report, tag in zip(reports, labels):
        lines.append(
            f"| {tag} | {report['geo_a']:.3f} | {report['geo_b']:.3f} | "
            f"{report['geo_delta_pct']:+.1f}% |"
        )
    lines.append("")
    append_summary("\n".join(lines) + "\n")
    return 0


def linux_reports(reports: list[dict]) -> list[dict]:
    out = []
    for report in reports:
        tag = label(report)
        if tag in ("linux-x64", "linux-arm"):
            out.append(report)
    return out


def decide(args: argparse.Namespace) -> int:
    """Keep/drop using the plan rule on linux-x64 and linux-arm."""
    reports = linux_reports(load_reports(Path(args.decide)))
    tags = sorted(label(r) for r in reports)
    if tags != ["linux-arm", "linux-x64"]:
        print("DECIDE: NEED_BOTH_LINUX")
        print("have:", [label(r) for r in reports])
        return 2
    slower_counts: dict[str, int] = {}
    reasons = []
    keep = True
    for report in reports:
        geo = report["geo_delta_pct"]
        faster5 = [
            n for n, s in report["workloads"].items()
            if s["median_a"] >= 5.0 and s["delta_pct"] < -5.0
        ]
        slower5 = [
            n for n, s in report["workloads"].items()
            if s["median_a"] >= 5.0 and s["delta_pct"] > 5.0
        ]
        for name in slower5:
            slower_counts[name] = slower_counts.get(name, 0) + 1
        geo_win = geo < -2.0
        targeted = abs(geo) <= 2.0 and bool(faster5) and not slower5
        if not (geo_win or targeted):
            keep = False
            reasons.append(
                f"{label(report)} geo {geo:+.1f}% faster5={faster5} slower5={slower5}"
            )
        else:
            reasons.append(
                f"{label(report)} geo {geo:+.1f}% ok faster5={faster5} slower5={slower5}"
            )
    both = [name for name, n in slower_counts.items() if n >= 2]
    if both:
        keep = False
        reasons.append(f"slower>5% on both linux: {both}")
    verdict = "KEEP" if keep else "DROP"
    print(f"DECIDE: {verdict}")
    for reason in reasons:
        print(" ", reason)
    return 0 if keep else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--out", default="bench.json")
    parser.add_argument("--binary", default="")
    parser.add_argument("--base-include", default="")
    parser.add_argument("--base-label", default="base")
    parser.add_argument(
        "--summarize",
        default="",
        help="Directory of downloaded bench.json artifacts; write a combined table.",
    )
    parser.add_argument(
        "--decide",
        default="",
        help="Directory of A/B bench.json artifacts; print KEEP/DROP for linux.",
    )
    args = parser.parse_args()
    if args.decide:
        return decide(args)
    if args.summarize:
        return summarize(args)
    if args.base_include:
        return ab_bench(args)
    return bench(args)


if __name__ == "__main__":
    sys.exit(main())
