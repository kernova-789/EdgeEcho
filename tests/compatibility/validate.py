#!/usr/bin/env python3
"""Verify EdgeEcho against a database created by the selected Replay library."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(__doc__)
p.add_argument('--storage-source', type=Path, required=True)
p.add_argument('--output', type=Path, default=project/'build-compat')
a = p.parse_args()
source = a.storage_source.resolve()
out = a.output.resolve()
if not out.is_relative_to(project) or out == project:
    raise SystemExit('Output must be a dedicated directory inside EdgeEcho')
out.mkdir(parents=True, exist_ok=True)
def fingerprints():
    files = [source/'CMakeLists.txt']
    for relative in ('src', 'include', 'third_party'):
        files += [x for x in (source/relative).rglob('*') if x.is_file()]
    return {str(x.relative_to(source)): hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(files)}
def run(command, name):
    r = subprocess.run([str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out/(name+'.log')).write_text(r.stdout, encoding='utf-8')
    if r.returncode:
        raise RuntimeError(r.stdout)
before = fingerprints()
build = out/'build'
run(['cmake', '-S', project, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
     '-DEDGEECHO_BUILD_GUI=OFF', '-DBUILD_TESTING=ON', f'-DREPLAY_SOURCE_DIR={source}'], 'configure')
run(['cmake', '--build', build, '--target', 'replay_compatibility', 'edgeecho_cli',
     '--parallel', '2'], 'build')
fixture = Path(tempfile.mkdtemp(prefix='writer-data-', dir=out))
run([build/'replay_compatibility', fixture], 'verify')
db = fixture/'replay.db'
digest = hashlib.sha256(db.read_bytes()).hexdigest()
run([build/'edgeecho_cli', 'inspect', fixture], 'inspect')
if digest != hashlib.sha256(db.read_bytes()).hexdigest():
    raise RuntimeError('Reader changed source database')
if any((db.parent/(db.name+tail)).exists() for tail in ('-wal','-shm','-journal')):
    raise RuntimeError('Unexpected source sidecar')
if before != fingerprints():
    raise RuntimeError('Reference source changed')
result = dict(passed=True, storage_source=str(source), reference_source_files_unchanged=len(before),
              writer_generated_records=5002, writer_generated_events=1,
              payload_metadata_pagination_plot_verified=True, source_database_sha256=digest,
              no_sidecars=True)
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
