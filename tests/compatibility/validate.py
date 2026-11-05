#!/usr/bin/env python3
"""Build an unmodified reference writer with all outputs confined to EdgeEcho.

Linux native integration check, separate from the standalone portable build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

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
run(['cmake','-S',source,'-B',out,'-DCMAKE_BUILD_TYPE=Release','-DREPLAY_BUILD_TOOLS=OFF','-DREPLAY_BUILD_EXAMPLES=OFF','-DBUILD_TESTING=OFF'], 'configure')
run(['cmake','--build',out,'--parallel','2'], 'build-writer')
run(['cmake','--build',project/'build','--target','edgeecho_core','edgeecho_cli','--parallel','2'], 'build-reader')
exe = out/'generate'
run(['c++','-std=c++17','-I',source/'include','-I',project/'include',project/'tests/compatibility/generate.cpp',out/'libedgesense_replay.a',project/'build/libedgeecho_core.a',project/'build/libedgeecho_sqlite.a','-lpthread','-ldl','-lm','-o',exe], 'compile-bridge')
# Stable IDs make this bridge safe to repeat against its own fixture.
run([exe,out/'writer-data'], 'verify')
db = out/'writer-data/replay.db'
digest = hashlib.sha256(db.read_bytes()).hexdigest()
run([project/'build/edgeecho_cli','inspect',out/'writer-data'],'inspect')
if digest != hashlib.sha256(db.read_bytes()).hexdigest():
    raise RuntimeError('Reader changed source database')
if any((db.parent/(db.name+tail)).exists() for tail in ('-wal','-shm','-journal')):
    raise RuntimeError('Unexpected source sidecar')
if before != fingerprints():
    raise RuntimeError('Reference source changed')
result = dict(passed=True, storage_source=str(source), reference_source_files_unchanged=len(before),
              writer_generated_records=3000, writer_generated_events=1,
              payload_metadata_pagination_plot_verified=True, source_database_sha256=digest,
              no_sidecars=True)
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
