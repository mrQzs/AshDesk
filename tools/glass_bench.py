"""Compare two built UI test bundles without changing either renderer.

Run from the repository root, with Qt on PATH or deployed beside each exe:
  python tools/glass_bench.py --before BEFORE --after AFTER --output OUTPUT

OUTPUT must be new. It retains complete stdout/stderr (including warmups),
GoogleTest XML, executable/dependency hashes, conditions and every measured run.
The images' KiB are retained backdrop buffers, NOT process RSS or peak memory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time


FILTER = "DesktopGlass.IdleRepaintReusesCachesWhileResizeAndScrollRecompose:GlassBenchmark.FixedPanelMovement"
HEADER = re.compile(
    r"\[ perf \] (\d+)x(\d+)@([\d.]+) backdrop ([\d.]+) ms, (\d+) KiB;"
    r"(?: panel caches (\d+) KiB;)? full glass frame ([\d.]+)/([\d.]+) ms"
)
STEPS = re.compile(
    r"\[ perf \] drag-resize step ([\d.]+)/([\d.]+) ms, scroll step ([\d.]+)/([\d.]+) ms,"
    r" panel composition p95 ([\d.]+) ms"
)
SMALL = re.compile(r"\[ perf \] 640x480 full frame p95: glass ([\d.]+) ms, solid ([\d.]+) ms")
FIXED = re.compile(r"\[ fixed \] panel (\d+)x(\d+)@([\d.]+) composition ([\d.]+)/([\d.]+) ms")
FIXED_SAMPLES = re.compile(r"\[ fixed samples ns \]([ \d]+)")
SOURCES = [
    "CMakeLists.txt", "ui/liquid_glass.cpp", "ui/liquid_glass.h",
    "tests/gtest/desktop_ui_test.cpp", "tests/test_glass.c",
    "tests/gtest/glass_benchmark_test.cpp",
    "core/kernels/ad_glass.c", "core/kernels/ad_glass.h",
    "core/kernels/ad_glass_x64.asm", "core/kernels/ad_glass_a64.S",
    "tools/glass_bench.py", "tools/build_glass_bench.ps1", "tools/check_glass.sh",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True, help="directory containing ashdesk_ui_tests")
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--platform", choices=("offscreen", "windows"), default="offscreen")
    parser.add_argument("--scale", default="1.75")
    args = parser.parse_args()
    if args.rounds < 3:
        parser.error("at least three measured rounds are required")
    suffix = ".exe" if os.name == "nt" else ""
    exes = {label: (directory / ("ashdesk_ui_tests" + suffix)).resolve()
            for label, directory in (("before", args.before), ("after", args.after))}
    for exe in exes.values():
        if not exe.is_file():
            parser.error(f"missing executable: {exe}")
        if os.name == "nt" and args.platform == "offscreen" and not (exe.parent / "platforms" / "qoffscreen.dll").is_file():
            parser.error(f"deploy Qt's platforms/qoffscreen.dll beside {exe}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = args.platform
    env["QT_SCALE_FACTOR"] = args.scale
    env.pop("ASHDESK_UI_SNAPSHOT_DIR", None)
    # Do not let an inherited plugin override load a different Qt into one variant.
    for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_SCREEN_SCALE_FACTORS"):
        env.pop(key, None)
    report = {
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "os": platform.platform(), "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "conditions": {"qpa": args.platform, "scale": args.scale, "filter": FILTER,
                       "rounds": args.rounds, "warmups_per_variant": 1,
                       "order": "AB, BA alternating", "timeout_seconds": 60},
        "memory_scope": "backdrop KiB = QImage sharp + blurred sizeInBytes; not RSS/peak",
        "binaries": {label: binary_manifest(exe) for label, exe in exes.items()},
        "sources": {name: digest(Path(name)) for name in SOURCES if Path(name).is_file()},
        "warmups": [], "runs": [],
    }
    manifest = exes["before"].parent.parent / "build.json"
    if manifest.is_file():
        report["build"] = json.loads(manifest.read_text(encoding="utf-8-sig"))
    try:
        dimensions = None
        for round_index in range(args.rounds + 1):
            labels = ("before", "after") if round_index % 2 == 0 else ("after", "before")
            for label in labels:
                stem = f"{round_index:02d}-{label}"
                result = run(exes[label], output, stem, env)
                result.update(variant=label, round=round_index)
                report["warmups" if round_index == 0 else "runs"].append(result)
                current = [result["window"], result["fixed_panel"]]
                if dimensions is None:
                    dimensions = current
                if current != dimensions:
                    raise RuntimeError(f"window/DPR changed: {dimensions} -> {current}")
                print(stem, json.dumps(result["metrics"], sort_keys=True), flush=True)
        report["summary"] = summarize(report["runs"])
        report["passed"] = True
    except Exception as error:
        report["passed"] = False
        report["error"] = str(error)
        raise
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def binary_manifest(exe):
    files = [exe] + sorted(exe.parent.rglob("*.dll"))
    return {str(path.relative_to(exe.parent)): digest(path) for path in files}


def run(exe, output, stem, env):
    started = time.perf_counter()
    xml = output / f"{stem}.xml"
    command = [str(exe), f"--gtest_filter={FILTER}", "--gtest_color=no",
               f"--gtest_output=xml:{xml}"]
    with (output / f"{stem}.stdout.txt").open("wb") as stdout, \
            (output / f"{stem}.stderr.txt").open("wb") as stderr:
        process = subprocess.run(command, env=env, stdout=stdout, stderr=stderr, timeout=60)
    text = (output / f"{stem}.stdout.txt").read_text(encoding="utf-8", errors="replace")
    if process.returncode != 0 or "[  PASSED  ] 2 tests." not in text:
        raise RuntimeError(f"{stem}: test failed (exit {process.returncode}); see full log")
    header, steps, small = HEADER.search(text), STEPS.search(text), SMALL.search(text)
    fixed, fixed_samples = FIXED.search(text), FIXED_SAMPLES.search(text)
    if not all((header, steps, small, fixed, fixed_samples)):
        raise RuntimeError(f"{stem}: missing performance output")
    metrics = dict(zip(
        ("backdrop_ms", "backdrop_kib", "idle_p50_ms", "idle_p95_ms"),
        map(float, (header[4], header[5], header[7], header[8]))
    ))
    metrics.update(zip(("drag_p50_ms", "drag_p95_ms", "scroll_p50_ms", "scroll_p95_ms",
                        "composition_p95_ms"), map(float, steps.groups())))
    metrics.update(zip(("small_glass_p95_ms", "small_solid_p95_ms"), map(float, small.groups())))
    if header[6] is not None:
        metrics["panel_cache_kib"] = float(header[6])
    metrics.update(fixed_composition_p50_ms=float(fixed[4]), fixed_composition_p95_ms=float(fixed[5]))
    return {"window": [int(header[1]), int(header[2]), float(header[3])],
            "fixed_panel": [int(fixed[1]), int(fixed[2]), float(fixed[3])],
            "fixed_composition_samples_ns": list(map(int, fixed_samples[1].split())),
            "elapsed_seconds": time.perf_counter() - started, "exit_code": process.returncode,
            "stdout": f"{stem}.stdout.txt", "stderr": f"{stem}.stderr.txt",
            "gtest_xml": xml.name, "metrics": metrics}


def summarize(runs):
    result = {}
    for label in ("before", "after"):
        samples = [run["metrics"] for run in runs if run["variant"] == label]
        result[label] = {}
        for metric in samples[0]:
            values = [sample[metric] for sample in samples]
            result[label][metric] = {"median": statistics.median(values),
                                     "min": min(values), "max": max(values), "samples": values}
    result["median_change_percent"] = {
        metric: 100 * (result["after"][metric]["median"] / before["median"] - 1)
        for metric, before in result["before"].items()
        if metric in result["after"] and before["median"] != 0
    }
    return result


if __name__ == "__main__":
    main()
