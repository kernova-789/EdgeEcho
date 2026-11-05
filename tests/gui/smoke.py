#!/usr/bin/env python3
"""Exercise Qt's real async loading/render path, Unicode paths and failure exits.

All caches, copied databases and results stay under --output. No X server needed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for stale in (output / "results.json", output / "interaction" / "interaction-result.json"):
        stale.unlink(missing_ok=True)
    source_dir = output / "input" / "中文 路径"
    source_dir.mkdir(parents=True, exist_ok=True)
    source = source_dir / "replay.db"
    shutil.copyfile(args.database, source)
    before = {p.name: digest(p) for p in source_dir.iterdir() if p.is_file()}
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    for name, relative in (("XDG_CACHE_HOME", "cache"), ("XDG_CONFIG_HOME", "config"),
                           ("XDG_DATA_HOME", "data"), ("XDG_RUNTIME_DIR", "run")):
        folder = output / relative
        folder.mkdir(exist_ok=True)
        folder.chmod(0o700)
        env[name] = str(folder)
    runtime = output / "run"
    for name in ("TMPDIR", "TMP", "TEMP"):
        env[name] = str(runtime)
    results = {}

    def run(name: str, path: Path, expected_success: bool, screenshot: bool = False) -> None:
        command = [str(args.exe.resolve()), "--smoke", str(path)]
        if screenshot:
            command.extend(("--screenshot", str(output / "unicode-smoke.png")))
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60, check=False)
        (output / f"{name}.log").write_text(result.stdout, encoding="utf-8")
        results[name] = {"returncode": result.returncode, "passed": (result.returncode == 0) == expected_success}
        if not results[name]["passed"]:
            raise RuntimeError(f"{name}: unexpected exit {result.returncode}\n{result.stdout}")
        if expected_success and "GUI smoke passed" not in result.stdout:
            raise RuntimeError("successful process did not complete GUI checks")

    run("unicode_offline", source, True, screenshot=True)
    after = {p.name: digest(p) for p in source_dir.iterdir() if p.is_file()}
    if before != after:
        raise RuntimeError("GUI changed input database or created files beside it")
    results["source_unchanged"] = {"passed": True, "sha256": before["replay.db"]}
    wal_dir = output / "input" / "nonempty-wal"
    wal_dir.mkdir(exist_ok=True)
    wal_source = wal_dir / "replay.db"
    shutil.copyfile(args.database, wal_source)
    wal = wal_dir / "replay.db-wal"
    wal.write_bytes(b"incomplete WAL must not be ignored")
    run("reject_nonempty_wal", wal_source, False)
    invalid = output / "input" / "invalid.db"
    invalid.write_bytes(b"this is not an SQLite database")
    run("reject_invalid_database", invalid, False)
    if not (output / "unicode-smoke.png").is_file():
        raise RuntimeError("GUI screenshot missing")
    # The interaction mode creates and mutates a dedicated fixture copy below
    # its output path. The caller-provided source stays untouched.
    interaction_output = output / "interaction"
    command = [str(args.exe.resolve()), "--interaction-test", str(source),
               "--test-output", str(interaction_output), "--screenshot", str(output / "interaction.png")]
    interaction = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=180, check=False)
    (output / "interaction.log").write_text(interaction.stdout, encoding="utf-8")
    evidence_path = interaction_output / "interaction-result.json"
    if interaction.returncode != 0 or not evidence_path.is_file():
        raise RuntimeError(f"interaction check failed ({interaction.returncode})\n{interaction.stdout}")
    interaction_evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if not interaction_evidence.get("passed") or "GUI interaction passed" not in interaction.stdout:
        raise RuntimeError("interaction executable did not finish its real-widget checks")
    results["interaction"] = {"passed": True, "returncode": interaction.returncode,
                              "checks": len(interaction_evidence["checks"])}
    after_interaction = {p.name: digest(p) for p in source_dir.iterdir() if p.is_file()}
    if after_interaction != before:
        raise RuntimeError("interaction check changed the original input fixture")
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
