#!/usr/bin/env python3
"""Trustworthy local interleaved A/B harness driver for canvas_ity.

Design (Phase A gate):
  - two snapshot include trees (base + ours), each with its own
    bench/local_impl.cpp TU compiled -std=c++03 -O2 -fno-exceptions
    -fno-rtti plus its own bench/local_bench.cpp harness TU (newer std),
    so each binary always matches its header's class layout.
  - warmup pass on A then B, then --pairs interleaved pairs (A then B)
    PER WORKLOAD, each process reporting best-of---trials for that one
    workload on a fresh canvas per trial.
  - per workload per side: min / median / max / spread=(max-min)/median.
  - gate: base median >= 10 ms, else DEMOTED (measured, never proof).
  - spread > 8% on either side: UNSTABLE, do not gate.
  - clean win: gated, stable, |delta| > 2% AND |delta| > 3x spread_a
    AND |delta| > 3x spread_b.
  - verdict KEEP_CANDIDATE: >=1 clean win on a gated workload, no gated
    workload slower by >5% or by >3x its own spread, geo-mean of gated
    medians not worse than +2%.  (Tests are run separately; a faster
    wrong picture is always DROP.)

Usage:
  python3 bench/local_ab.py --base 8be29cb --out /tmp/ab.json
  python3 bench/local_ab.py --base-dir /path/to/tree --pairs 7 --trials 7
  python3 bench/local_ab.py --base main --ours-dir /path --workloads pattern_tiled,image_scaled
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
WORKLOADS = [
    "path_construction", "flattening_fill", "fill_small", "fill_large",
    "fill_zone_plate", "stroke_many", "stroke_wide", "gradient_linear",
    "gradient_radial", "pattern_tiled", "image_scaled", "clip_heavy",
    "composite_ops", "shadow_blurred", "transforms", "many_primitives",
    "complex_scene",
]
GATE_MS = 10.0
SPREAD_LIMIT = 0.08


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run(cmd, cwd=None):
    # type: (list, Path | None) -> str
    result = subprocess.run(
        [str(c) for c in cmd], cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            "command failed (%d): %s\n%s" % (
                result.returncode, " ".join(str(c) for c in cmd),
                result.stdout[-4000:]))
    return result.stdout


def banner(compiler):
    # type: (str) -> str
    # NB: bare cl exits nonzero after printing its banner, so tolerate
    # the exit code there.
    try:
        if compiler == "cl":
            proc = subprocess.run(
                [compiler], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True)
            out = proc.stdout
        else:
            out = run([compiler, "--version"])
    except OSError:
        return compiler
    for line in out.splitlines():
        if line.strip():
            return line.strip()
    return compiler


def compiler_label(banner_text):
    # type: (str) -> str
    # Turn raw first lines like
    #   "c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0" or
    #   "Apple clang version 21.0.0 (clang-2100.1.1.101)" or
    #   "Microsoft (R) C/C++ Optimizing Compiler Version 19.44 ..."
    # into short "GCC 13.3.0 (Ubuntu 24.04)" / "Apple Clang 21.0.0" /
    # "MSVC 19.44" labels for reports.
    text = (banner_text or "").strip()
    match = re.match(r"Apple clang version (\d+(?:\.\d+)+)", text)
    if match:
        return "Apple Clang " + match.group(1)
    match = re.search(r"Optimizing Compiler Version (\d+\.\d+)", text)
    if match:
        return "MSVC " + match.group(1)
    match = re.match(
        r"(?:c\+\+|g\+\+|gcc|clang)(?: version)?(?: \(([^)]*)\))?"
        r" (\d+(?:\.\d+)+)", text)
    if match:
        distro, version = match.group(1) or "", match.group(2)
        family = "Clang" if text.startswith("clang") else "GCC"
        suffix = ""
        name = re.match(r"([A-Za-z]+)", distro)
        rel = re.search(r"~(\d+\.\d+)", distro)
        if name:
            suffix = " (" + name.group(1)
            suffix += " " + rel.group(1) if rel else ""
            suffix += ")"
        return family + " " + version + suffix
    return text.splitlines()[0][:64] if text else "unknown compiler"


def short_sha(value):
    # type: (str) -> str
    return value[:12] if value else value


def median(values):
    # type: (list) -> float
    ordered = sorted(values)
    n = len(ordered)
    if n == 0:
        return float("nan")
    mid = n // 2
    if n % 2:
        return ordered[mid]
    return 0.5 * (ordered[mid - 1] + ordered[mid])


def snapshot_tree(root, dest, ref=None, src_dir=None):
    # type: (Path, Path, str | None, Path | None) -> str
    dest.mkdir(parents=True, exist_ok=True)
    header = dest / "canvas_ity.hpp"
    if src_dir is not None:
        shutil.copyfile(str(src_dir / "canvas_ity.hpp"), str(header))
        return "dir:%s" % src_dir
    out = run(["git", "show", "%s:src/canvas_ity.hpp" % ref], cwd=root)
    header.write_text(out, encoding="utf-8")
    sha = run(["git", "rev-parse", ref], cwd=root).strip()
    return sha


def parse_time(text):
    # type: (str) -> float
    for line in text.splitlines():
        match = LINE.match(line.strip())
        if match:
            return float(match.group(2))
    raise RuntimeError("no workload time parsed:\n" + text[-800:])


def summarize(samples):
    # type: (list) -> dict
    lo = min(samples)
    hi = max(samples)
    mid = median(samples)
    spread = (hi - lo) / mid if mid else float("inf")
    return {"min_ms": round(lo, 3), "median_ms": round(mid, 3),
            "max_ms": round(hi, 3), "spread": round(spread, 4),
            "n": len(samples)}


def geo_mean(values):
    # type: (list) -> float
    if not values or any(v <= 0 for v in values):
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def render_pngs(binary, args, names, report_path):
    # type: (Path, object, list, Path) -> str
    # Render each workload with the B tree into --renders dir (or next
    # to the JSON report when the flag is given without a value).
    if args.renders is None:
        return ""
    dest = Path(args.renders) if args.renders else         report_path.parent / "renders"
    dest.mkdir(parents=True, exist_ok=True)
    run([str(binary), "render", str(dest)])
    return str(dest)


def single_report(args, names, collected_b, ours_id, ours_sha, dirty,
                    comp_banner, lib_flags, harness_flags):
    # type: (object, list, dict, str, str, str, str, list, list) -> int
    workloads = {}
    medians = []
    for name in names:
        sb = summarize(collected_b[name])
        workloads[name] = {
            "samples": [round(v, 3) for v in collected_b[name]],
            "median_ms": sb["median_ms"],
            "min_ms": sb["min_ms"], "max_ms": sb["max_ms"],
            "spread": sb["spread"],
            "gated": sb["median_ms"] >= GATE_MS,
            "stable": sb["spread"] <= SPREAD_LIMIT,
        }
        if sb["median_ms"] >= GATE_MS:
            medians.append(sb["median_ms"])
    geo = geo_mean(medians)
    report = {
        "mode": "local_single",
        "os": platform.system(), "arch": platform.machine(),
        "compiler": compiler_label(comp_banner),
        "compiler_raw": comp_banner,
        "lib_flags": " ".join(lib_flags),
        "harness_flags": " ".join(harness_flags),
        "ours_id": ours_id, "ours_sha": ours_sha,
        "ours_dirty": dirty,
        "pairs": args.pairs, "trials": args.trials,
        "gate_ms": GATE_MS, "spread_limit": SPREAD_LIMIT,
        "geo_ms": round(geo, 3),
        "workloads": workloads,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    renders = render_pngs(bin_b, args, names, out)
    report["renders"] = renders
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    rows = ["# Local bench (%s %s)" % (report["os"], report["arch"]), "",
            "- compiler: `%s`" % compiler_label(comp_banner),
            "- lib: `%s`  harness: `%s`" % (report["lib_flags"],
                                            report["harness_flags"]),
            "- ours: `%s%s`" % (short_sha(ours_sha), " DIRTY" if dirty else ""),
            "- %d runs per workload after warmup, best-of-%d" % (
                args.pairs, args.trials),
            "- gated geo: %.3f ms" % geo, "",
            "- renders (one PNG per workload): %s" % (
                renders if renders else "not requested"), "",
            "| workload | median ms | spread | gated |",
            "|---|---:|---:|---|"]
    for name in names:
        s = workloads[name]
        rows.append(
            "| {n} | {m:.3f} | {sp:.1%} | {g} |".format(
                n=name, m=s["median_ms"], sp=s["spread"],
                g="yes" if s["gated"] else "no"))
    print("\n".join(rows) + "\n", flush=True)
    return 0


def main():
    # type: () -> int
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="8be29cb",
                        help="git ref for the A tree (ignored with --base-dir)")
    parser.add_argument("--base-dir", default="",
                        help="directory holding canvas_ity.hpp for A tree")
    parser.add_argument("--ours-dir", default="",
                        help="directory holding canvas_ity.hpp for B tree "
                             "(default: snapshot of working src/)")
    parser.add_argument("--pairs", type=int, default=7)
    parser.add_argument("--trials", type=int, default=9)
    parser.add_argument("--workloads", default="",
                        help="comma-separated subset (default: all)")
    parser.add_argument("--out", default="/tmp/ab.json")
    parser.add_argument("--build-dir", default="bench/build/local_ab")
    parser.add_argument("--single", action="store_true",
                        help="report the B tree alone (median/spread/geo, "
                             "no A/B deltas); used for main-push reports")
    parser.add_argument("--renders", default="",
                        help="directory to write one PNG per workload "
                             "(rendered with the B tree)")
    args = parser.parse_args()

    root = repo_root()
    build = (root / args.build_dir).resolve()
    build.mkdir(parents=True, exist_ok=True)
    tree_a = build / "tree_a"
    tree_b = build / "tree_b"

    base_id = ""
    if not args.single:
        base_id = snapshot_tree(
            root, tree_a,
            src_dir=Path(args.base_dir) if args.base_dir else None,
            ref=None if args.base_dir else args.base)
    if args.ours_dir:
        ours_id = snapshot_tree(
            root, tree_b, src_dir=Path(args.ours_dir))
    else:
        ours_id = snapshot_tree(root, tree_b, src_dir=root / "src")
    try:
        ours_sha = run(["git", "rev-parse", "HEAD"], cwd=root).strip()
        dirty = run(["git", "status", "--short", "src/"],
                    cwd=root).strip()
    except RuntimeError:
        ours_sha, dirty = "unknown", ""

    # All paths below are absolute, so every compile and run works no
    # matter what the process working directory is (MSVC on CI runs with
    # bench/build as its cwd).
    if os.name == "nt":
        compiler = "cl"
        lib_flags = ["/O2", "/EHsc"]
        harness_flags = ["/O2", "/EHsc"]
        obj_ext = ".obj"
        bin_ext = ".exe"
    else:
        compiler = "c++"
        lib_flags = ["-O2", "-std=c++03", "-fno-exceptions", "-fno-rtti"]
        harness_flags = ["-O2", "-std=c++11"]
        obj_ext = ".o"
        bin_ext = ""
    comp_banner = banner(compiler)

    def compile_one(src, inc, obj, flags):
        # type: (Path, Path, Path, list) -> None
        src, inc, obj = str(src), str(inc), str(obj)
        if os.name == "nt":
            run([compiler, "/nologo"] + flags + ["/c", "/I" + inc,
                                                 "/Fo" + obj, src])
        else:
            run([compiler] + flags + ["-I", inc, "-c", src, "-o", obj])

    def link(binpath, *objs):
        # type: (Path, object) -> None
        parts = [str(o) for o in objs]
        if os.name == "nt":
            run([compiler, "/nologo", "/O2", "/EHsc"] + parts +
                ["/Fe" + str(binpath)])
        else:
            run([compiler, "-O2", "-o", str(binpath)] + parts)

    harness_src = root / "bench" / "local_bench.cpp"
    impl_src = root / "bench" / "local_impl.cpp"
    harness_a_o = build / ("harness_a" + obj_ext)
    harness_b_o = build / ("harness_b" + obj_ext)
    impl_a_o = build / ("impl_a" + obj_ext)
    impl_b_o = build / ("impl_b" + obj_ext)
    bin_a = build / ("bench_a" + bin_ext)
    bin_b = build / ("bench_b" + bin_ext)

    # Each binary gets a harness TU compiled against its OWN tree.  The
    # two headers may declare different class layouts (extra scratch
    # members), so sharing one harness object would size stack canvases
    # with the wrong header when the A tree is larger than the B tree.
    print("compile harness A+B: %s" % compiler_label(comp_banner), flush=True)
    if not args.single:
        compile_one(harness_src, tree_a, harness_a_o, harness_flags)
    compile_one(harness_src, tree_b, harness_b_o, harness_flags)
    if not args.single:
        print("compile lib A: %s" % " ".join(lib_flags), flush=True)
        compile_one(impl_src, tree_a, impl_a_o, lib_flags)
    print("compile lib B: %s" % " ".join(lib_flags), flush=True)
    compile_one(impl_src, tree_b, impl_b_o, lib_flags)
    if not args.single:
        link(bin_a, harness_a_o, impl_a_o)
    link(bin_b, harness_b_o, impl_b_o)

    names = [w for w in WORKLOADS
             if not args.workloads or w in args.workloads.split(",")]
    if not names:
        raise SystemExit("no workloads selected")

    def once(binary, workload):
        # type: (Path, str) -> float
        return parse_time(run([str(binary), str(args.trials), workload]))

    for lap in (1, 2):
        if not args.single:
            print("warmup A (all workloads, lap %d/2)" % lap, flush=True)
            for name in names:
                once(bin_a, name)
        print("warmup B (all workloads, lap %d/2)" % lap, flush=True)
        for name in names:
            once(bin_b, name)

    # Per-workload pair blocks: all pairs for one workload run back to
    # back (A,B interleaved), so one workload's sample set spans seconds,
    # not the minutes a whole-suite sweep would take.  Slow thermal drift
    # then cannot inflate (max-min)/median spreads.
    collected_a = {name: [] for name in names}  # type: dict
    collected_b = {name: [] for name in names}  # type: dict
    for name in names:
        for pair in range(args.pairs):
            if not args.single:
                collected_a[name].append(once(bin_a, name))
            collected_b[name].append(once(bin_b, name))
        print("done %s" % name, flush=True)

    if args.single:
        return single_report(args, names, collected_b, ours_id, ours_sha,
                             dirty, comp_banner, lib_flags, harness_flags)

    workloads = {}
    gated_a, gated_b = [], []
    wins, losses, vetoes, unstable, demoted = [], [], [], [], []
    for name in names:
        sa = summarize(collected_a[name])
        sb = summarize(collected_b[name])
        base = sa["median_ms"]
        ours = sb["median_ms"]
        delta = 100.0 * (ours - base) / base if base else float("nan")
        gated = base >= GATE_MS
        stable = sa["spread"] <= SPREAD_LIMIT and sb["spread"] <= SPREAD_LIMIT
        clean = (gated and stable and abs(delta) > 2.0
                 and abs(delta) > 3.0 * sa["spread"] * 100.0
                 and abs(delta) > 3.0 * sb["spread"] * 100.0)
        if gated and stable:
            gated_a.append(base)
            gated_b.append(ours)
        if not gated:
            status, demoted = "DEMOTED", demoted + [name]
        elif not stable:
            status, unstable = "UNSTABLE", unstable + [name]
        elif clean and delta < 0:
            status, wins = "CLEAN_WIN", wins + [name]
        elif clean:
            status, losses = "CLEAN_LOSS", losses + [name]
        else:
            status = "NOISE"
        if gated and (delta > 5.0
                      or delta > 3.0 * max(sa["spread"], sb["spread"]) * 100.0):
            vetoes.append(name)
        workloads[name] = {
            "samples_a": [round(v, 3) for v in collected_a[name]],
            "samples_b": [round(v, 3) for v in collected_b[name]],
            "median_a": base, "median_b": ours,
            "delta_pct": round(delta, 2),
            "min_a": sa["min_ms"], "max_a": sa["max_ms"],
            "spread_a": sa["spread"],
            "min_b": sb["min_ms"], "max_b": sb["max_ms"],
            "spread_b": sb["spread"],
            "gated": gated, "stable": stable,
            "clean": bool(clean), "status": status,
        }

    geo_a = geo_mean(gated_a)
    geo_b = geo_mean(gated_b)
    geo_delta = 100.0 * (geo_b - geo_a) / geo_a if gated_a else float("nan")
    keep = bool(wins) and not vetoes and geo_delta <= 2.0
    report = {
        "mode": "local_ab",
        "os": platform.system(), "arch": platform.machine(),
        "compiler": compiler_label(comp_banner),
        "compiler_raw": comp_banner,
        "lib_flags": " ".join(lib_flags),
        "harness_flags": " ".join(harness_flags),
        "pinning": "none (Darwin has no core pinning; "
                   "interleaved per-workload pairs + best-of-%d)" % args.trials,
        "base_id": base_id, "ours_id": ours_id, "ours_sha": ours_sha,
        "ours_dirty": dirty,
        "pairs": args.pairs, "trials": args.trials,
        "gate_ms": GATE_MS, "spread_limit": SPREAD_LIMIT,
        "geo_a": round(geo_a, 3), "geo_b": round(geo_b, 3),
        "geo_delta_pct": round(geo_delta, 2),
        "keep_candidate": keep,
        "clean_wins": wins, "clean_losses": losses,
        "vetoes": vetoes, "unstable": unstable, "demoted": demoted,
        "workloads": workloads,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    renders = render_pngs(bin_b, args, names, out)
    report["renders"] = renders
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    rows = ["# Local A/B (%s %s)" % (report["os"], report["arch"]), "",
            "- compiler: `%s`" % compiler_label(comp_banner),
            "- lib: `%s`  harness: `%s`" % (report["lib_flags"],
                                            report["harness_flags"]),
            "- base: `%s`  ours: `%s%s`" % (
                short_sha(base_id), short_sha(ours_sha),
                " DIRTY" if dirty else ""),
            "- %d interleaved pairs per workload after warmup, best-of-%d" % (
                args.pairs, args.trials),
            "- renders (one PNG per workload): %s" % (
                renders if renders else "not requested"),
            "- gated geo: **%+.1f%%** (base %.3f ms -> ours %.3f ms)" % (
                geo_delta, geo_a, geo_b),
            "- verdict: **%s** wins=%s vetoes=%s unstable=%s demoted=%s" % (
                "KEEP_CANDIDATE" if keep else "DROP",
                wins, vetoes, unstable, demoted), "",
            "| workload | base med | ours med | delta | spread A | spread B | status |",
            "|---|---:|---:|---:|---:|---:|---|"]
    for name in names:
        s = workloads[name]
        rows.append(
            "| {n} | {a:.3f} | {b:.3f} | {d:+.1f}% | {sa:.1%} | {sb:.1%} | {st} |".format(
                n=name, a=s["median_a"], b=s["median_b"], d=s["delta_pct"],
                sa=s["spread_a"], sb=s["spread_b"], st=s["status"]))
    print("\n".join(rows) + "\n", flush=True)
    return 0 if keep else 1


if __name__ == "__main__":
    sys.exit(main())
