#!/usr/bin/env python3
"""Reproducible Linux CLI performance run; writes only below EdgeEcho."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import sqlite3
import struct
import subprocess
import sys
import time


WORKSPACE = Path(__file__).resolve().parents[2]
ANCHOR = 1793145600000000
SCALARS = 200000
BLOCKS = 250
SAMPLES = 32768
SENSOR = "performance-vibration"
TYPE = "vibration_iio_block"
MARKER = ".edgeecho-performance-output"


def contained(path):
    resolved = Path(path).resolve()
    try:
        resolved.relative_to(WORKSPACE)
    except ValueError as error:
        raise ValueError(f"Output must be inside {WORKSPACE}: {resolved}") from error
    if resolved == WORKSPACE:
        raise ValueError("Output must be a child directory, not the workspace root")
    return resolved


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run_process(label, command, output, measure=False):
    stdout = output / (label + ".stdout.json")
    stderr = output / (label + ".stderr.txt")
    resource = output / (label + ".resource.json")
    # A failed rerun must never reuse an earlier process's measurements.
    if resource.exists():
        resource.unlink()
    executed = list(command)
    if measure:
        executed = ["/usr/bin/time", "-f",
                    '{"max_rss_kib":%M,"process_elapsed_seconds":%e,'
                    '"user_seconds":%U,"system_seconds":%S,"exit_code":%x}',
                    "-o", str(resource)] + executed
    started = time.perf_counter()
    timed_out = False
    with stdout.open("w", encoding="utf-8") as out, stderr.open("w", encoding="utf-8") as err:
        try:
            result = subprocess.run(executed, cwd=WORKSPACE, stdout=out, stderr=err,
                                    check=False, timeout=90, env={**os.environ, "LC_ALL": "C"})
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = None
            timed_out = True
    elapsed = time.perf_counter() - started
    record = {"command": list(command), "exit_code": code, "timed_out": timed_out,
              "runner_wall_seconds": elapsed, "stdout": str(stdout.relative_to(WORKSPACE)),
              "stderr": str(stderr.relative_to(WORKSPACE))}
    if measure and resource.exists():
        # GNU time prepends an explanatory line for unsuccessful children.
        for line in resource.read_text(encoding="utf-8").splitlines():
            if line.startswith("{"):
                record["process_resource"] = json.loads(line)
    text = stdout.read_text(encoding="utf-8")
    if code == 0:
        record["result"] = json.loads(text)
    else:
        record["error"] = stderr.read_text(encoding="utf-8").strip()
        record["recommendation"] = "Narrow the range if the default 15-second query budget was exceeded."
    return record


def generate_workload(cli, database, output):
    demo = run_process("generate-demo", [str(cli), "demo", str(database), "--records", str(SCALARS)], output)
    if demo["exit_code"] != 0:
        raise RuntimeError("CLI demo generation failed; see recorded logs")
    # Fixed bounded payload reused across blocks; this is synthetic raw-count
    # traffic, not hardware-acquired transport or timestamp validation.
    payload = bytearray(SAMPLES * 24)
    for sample in range(SAMPLES):
        x = ((sample * 257) % 65536) - 32768
        y = ((sample * 131) % 65536) - 32768
        z = ((sample * 67) % 65536) - 32768
        struct.pack_into("<hhh2xIH2xq", payload, sample * 24, x, y, z,
                         sample // 64, sample % 64, ANCHOR * 1000 + sample * 1000000000 // SAMPLES)
    started = time.perf_counter()
    with sqlite3.connect(database) as connection:
        connection.execute("PRAGMA journal_mode=DELETE")
        connection.executemany(
            "INSERT INTO records(id,sensor,type,unit,ts,kind,scalar,block,quality,duration,rate,"
            "encoding,source,batch,metadata,version) VALUES(?,?,?,?,?,2,NULL,?,0,1000000,?,"
            "'iio-le-24-v1','EdgeEcho synthetic performance',?,'12:input_origin9:simulated',1)",
            ((f"performance-block-{number:04d}", SENSOR, TYPE, "count", ANCHOR + number * 1000000,
              payload, SAMPLES, f"synthetic-batch-{number:04d}") for number in range(BLOCKS)))
        connection.execute("UPDATE aggregate_state SET backfill_end=(SELECT coalesce(max(seq),0) "
                           "FROM records),backfill_cursor=0")
    return demo, time.perf_counter() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="Fresh or runner-owned output directory inside EdgeEcho")
    parser.add_argument("--cli", type=Path, default=WORKSPACE / "build/edgeecho_cli")
    parser.add_argument("--report", type=Path,
                        default=WORKSPACE / "docs/evidence/performance.json")
    parser.add_argument("--reuse", action="store_true",
                        help="Read an existing runner-owned workload without regenerating it")
    args = parser.parse_args()
    output = contained(args.output)
    report_path = contained(args.report)
    cli = args.cli.resolve()
    if not cli.is_file() or not Path("/usr/bin/time").is_file():
        raise ValueError("Built CLI and Linux /usr/bin/time are required")
    if output.exists() and any(output.iterdir()) and not (output / MARKER).is_file():
        raise ValueError("Refusing to use a nonempty directory not owned by this runner")
    if args.reuse and (not (output / MARKER).is_file() or not (output / "performance.db").is_file()):
        raise ValueError("--reuse requires an existing runner-owned performance.db")
    output.mkdir(parents=True, exist_ok=True)
    (output / MARKER).write_text("EdgeEcho synthetic performance output\n", encoding="utf-8")
    database = output / "performance.db"
    for suffix in ("", "-wal", "-shm", "-journal"):
        candidate = database.with_name(database.name + suffix)
        if candidate.is_symlink():
            raise ValueError("Refusing symlink in runner-owned database output")
        if candidate.exists() and not args.reuse:
            candidate.unlink()
        elif args.reuse and suffix and candidate.exists():
            raise ValueError("--reuse requires a closed workload without sidecars")

    binary_sha = digest(cli)
    source_files = ["src/database.cpp", "src/codec.cpp", "src/playback.cpp", "tools/cli.cpp"]
    source_sha = {name: digest(WORKSPACE / name) for name in source_files}
    cpu = "unknown"
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    report = {
        "recorded_at": datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),
        "timezone": "Asia/Shanghai", "platform": platform.platform(),
        "uname": list(platform.uname()), "cpu_model": cpu, "logical_cpus": os.cpu_count(),
        "scope": "Linux host synthetic CLI performance; not physical-board or Windows validation",
        "cache_note": "Sequential runs; OS filesystem cache uncontrolled; no cold-cache claim",
        "rss_method": "GNU /usr/bin/time %M: peak resident set of each fresh CLI process, in KiB",
        "timing_note": "CLI elapsed_ms measures Database::plot only; process timings include opening and overview",
        "default_query_timeout_ms": 15000,
        "binary": str(cli.relative_to(WORKSPACE)) if cli.is_relative_to(WORKSPACE) else str(cli),
        "binary_sha256": binary_sha, "source_sha256": source_sha,
        "workload": {"scalar_records": SCALARS, "added_waveform_blocks": BLOCKS,
                     "samples_per_block": SAMPLES, "waveform_points": BLOCKS * SAMPLES,
                     "waveform_encoding": "iio-le-24-v1", "sample_rate_hz": SAMPLES,
                     "waveform_block_duration_us": 1000000, "synthetic": True},
    }
    if args.reuse:
        report["generation"] = {"reused": True, "note": "Existing runner-owned synthetic workload; not rewritten"}
    else:
        demo, append_seconds = generate_workload(cli, database, output)
        report["generation"] = demo
        report["append_seconds"] = append_seconds
    connection = sqlite3.connect(database.as_uri() + "?mode=ro&immutable=1", uri=True)
    try:
        total = connection.execute("SELECT count(*) FROM records").fetchone()[0]
        scalar_range = connection.execute("SELECT min(ts),max(ts)+1 FROM records WHERE kind=1").fetchone()
        scalar_count = connection.execute("SELECT count(*) FROM records WHERE kind=1").fetchone()[0]
        wave_count, wave_bytes = connection.execute("SELECT count(*),sum(length(block)) FROM records WHERE sensor=? AND type=?",
                                                    (SENSOR, TYPE)).fetchone()
        integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
    finally:
        connection.close()
    if scalar_count != SCALARS or wave_count != BLOCKS or wave_bytes != BLOCKS * SAMPLES * 24 or total != SCALARS + 60 + BLOCKS:
        raise ValueError("Synthetic workload counts do not match the reproducible fixture")
    report["database"] = {"path": str(database.relative_to(WORKSPACE)), "size_bytes": database.stat().st_size,
                          "records": total, "sqlite_integrity_check": integrity}
    before = digest(database)
    sidecars_before = [suffix for suffix in ("-wal", "-shm", "-journal")
                       if database.with_name(database.name + suffix).exists()]
    common = [str(cli), "benchmark", str(database)]
    cases = [
        ("waveform-full", SENSOR, TYPE, ANCHOR, ANCHOR + BLOCKS * 1000000, "x"),
        ("scalar-full", "demo-vibration", "acceleration_rms", scalar_range[0], scalar_range[1], "value"),
        ("scalar-tail-one-second", "demo-vibration", "acceleration_rms",
         scalar_range[1] - 1000000, scalar_range[1], "value"),
    ]
    benchmarks = {}
    for name, sensor, data_type, start, end, channel in cases:
        command = common + ["--sensor", sensor, "--type", data_type, "--unit", "count",
                            "--start", str(start), "--end", str(end),
                            "--channel", channel, "--buckets", "1500"]
        benchmarks[name] = run_process(name, command, output, measure=True)
    report["benchmarks"] = benchmarks
    report["source_safety"] = {"database_sha256_before": before, "database_sha256_after": digest(database),
                               "sidecars_before": sidecars_before,
                               "sidecars_after": [suffix for suffix in ("-wal", "-shm", "-journal")
                                                  if database.with_name(database.name + suffix).exists()]}
    report["source_safety"]["unchanged"] = (
        report["source_safety"]["database_sha256_before"] == report["source_safety"]["database_sha256_after"]
        and not report["source_safety"]["sidecars_before"] and not report["source_safety"]["sidecars_after"])
    report["binary_unchanged"] = binary_sha == digest(cli)
    report["source_unchanged"] = source_sha == {name: digest(WORKSPACE / name) for name in source_files}
    report["all_benchmarks_succeeded"] = all(value["exit_code"] == 0 for value in benchmarks.values())
    wave = benchmarks["waveform-full"].get("result", {})
    report["waveform_count_matches"] = wave.get("records_scanned") == BLOCKS and wave.get("samples_decoded") == BLOCKS * SAMPLES
    report["passed"] = all(report[name] for name in ("all_benchmarks_succeeded", "waveform_count_matches",
                                                    "binary_unchanged", "source_unchanged")) and report["source_safety"]["unchanged"]
    save(report_path, report)
    save(output / "report.json", report)
    print(json.dumps({"report": str(report_path.relative_to(WORKSPACE)), "passed": report["passed"],
                      "database_bytes": report["database"]["size_bytes"],
                      "benchmarks": {name: {"result": result.get("result"),
                                             "resource": result.get("process_resource"),
                                             "error": result.get("error")} for name, result in benchmarks.items()}},
                     ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"performance runner: {error}", file=sys.stderr)
        sys.exit(1)
