#!/usr/bin/env python3
"""Create a relocatable Linux Qt runtime bundle using the current host ABI.

Run from a trusted local build. This is not an AppImage or a promise of an
older glibc baseline. All generated files stay under the selected project path.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile

from validation_support import assert_sources_unchanged, executable_identity, fingerprints, source_id, version

ROOT = Path(__file__).resolve().parents[1]
SYSTEM_ABI = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2',
              'librt.so.1', 'libresolv.so.2', 'libutil.so.1', 'libanl.so.1'}

def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs).stdout

def within_project(path: Path) -> Path:
    path = path.resolve()
    if path == ROOT or not path.is_relative_to(ROOT):
        raise RuntimeError('Package outputs must use a dedicated directory inside EdgeEcho')
    return path

def dependencies(path: Path):
    result = {}
    for line in run(['ldd', path]).splitlines():
        if 'not found' in line:
            raise RuntimeError(f'Unresolved library for {path}: {line}')
        match = re.search(r'^\s*(\S+) => (/.+?) \(', line)
        if match:
            result[match[1]] = Path(match[2])
    return result

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT/'build')
    parser.add_argument('--output', type=Path, default=ROOT/'dist')
    parser.add_argument('--qt-plugins', type=Path)
    parser.add_argument('--patchelf', type=Path)
    args = parser.parse_args()
    output = within_project(args.output)
    output.mkdir(parents=True, exist_ok=True)
    build = args.build_dir.resolve()
    executables = [build/'edgeecho', build/'edgeecho_cli']
    if any(not p.is_file() for p in executables):
        raise RuntimeError('Build both desktop and CLI before packaging')
    source_before = fingerprints(ROOT)
    expected_identity = (version(ROOT), source_id(source_before))
    version_text = run([executables[1], '--version']).strip()
    for source in executables:
        if executable_identity(run([source, '--version'])) != expected_identity:
            raise RuntimeError('Refusing to package stale/mixed-build executables: ' + str(source))
    bundle_name = f'EdgeEcho-{expected_identity[0]}-linux-{platform.machine()}'
    stage = output/bundle_name
    if stage.exists():
        if not (stage/'.edgeecho-package').is_file():
            raise RuntimeError('Refusing to replace a directory not owned by this packager')
        shutil.rmtree(stage)
    stage.mkdir()
    (stage/'.edgeecho-package').write_text('EdgeEcho runtime bundle\n')
    for relative in ('bin', 'lib', 'plugins/platforms', 'share/doc', 'share/licenses'):
        (stage/relative).mkdir(parents=True)
    patch = args.patchelf or shutil.which('patchelf') or ROOT/'.deps/patchelf/usr/bin/patchelf'
    if not Path(patch).is_file():
        raise RuntimeError('patchelf is required to remove build-machine runtime paths')
    plugin_root = args.qt_plugins
    if plugin_root is None:
        qt_core = dependencies(executables[0]).get('libQt6Core.so.6')
        candidates = ([qt_core.resolve().parent.parent/'plugins'] if qt_core else []) + [Path('/usr/lib')/platform.machine().replace('x86_64','x86_64-linux-gnu')/'qt6/plugins',
                      Path('/usr/lib/x86_64-linux-gnu/qt6/plugins')]
        plugin_root = next((p for p in candidates if (p/'platforms/libqoffscreen.so').is_file()), None)
    if plugin_root is None:
        raise RuntimeError('Specify --qt-plugins for this Qt installation')
    seeds = list(executables)
    for plugin in ('libqoffscreen.so', 'libqxcb.so', 'libqminimal.so'):
        source = plugin_root/'platforms'/plugin
        if not source.is_file():
            raise RuntimeError(f'Missing required Qt platform plugin: {source}')
        shutil.copy2(source, stage/'plugins/platforms'/plugin)
        seeds.append(source)
    libraries = {}
    for seed in seeds:
        libraries.update(dependencies(seed))
    omitted = sorted(name for name in libraries if name in SYSTEM_ABI or name.startswith('ld-linux'))
    libraries = {name:path for name,path in libraries.items() if name not in omitted}
    for source in executables:
        target = stage/'bin'/source.name
        shutil.copy2(source, target)
        run([patch, '--set-rpath', '$ORIGIN/../lib', target])
    for name, source in libraries.items():
        target = stage/'lib'/name
        shutil.copy2(source.resolve(), target)
        run([patch, '--set-rpath', '$ORIGIN', target])
    for target in (stage/'plugins/platforms').glob('*.so'):
        run([patch, '--set-rpath', '$ORIGIN/../../lib', target])
    (stage/'bin/qt.conf').write_text('[Paths]\nPrefix=..\nPlugins=plugins\nLibraries=lib\nTranslations=translations\n')
    launcher = '''#!/bin/sh
set -eu
bundle_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$bundle_root/lib"
export QT_PLUGIN_PATH="$bundle_root/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$bundle_root/plugins/platforms"
unset QML2_IMPORT_PATH QML_IMPORT_PATH
export XDG_CACHE_HOME="$bundle_root/.runtime/cache"
export XDG_CONFIG_HOME="$bundle_root/.runtime/config"
export XDG_DATA_HOME="$bundle_root/.runtime/data"
export XDG_RUNTIME_DIR="$bundle_root/.runtime/run"
mkdir -p "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export TMPDIR="$XDG_RUNTIME_DIR"
export TMP="$XDG_RUNTIME_DIR"
export TEMP="$XDG_RUNTIME_DIR"
exec "$bundle_root/bin/edgeecho" "$@"
'''
    (stage/'edgeecho').write_text(launcher)
    (stage/'edgeecho').chmod(0o755)
    (stage/'edgeecho_cli').write_text(launcher.replace('bin/edgeecho"', 'bin/edgeecho_cli"'))
    (stage/'edgeecho_cli').chmod(0o755)
    for file in ('README.md', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(ROOT/file, stage/'share/doc'/file)
    shutil.copytree(ROOT/'docs', stage/'share/doc/docs')
    shutil.copytree(ROOT/'third_party/licenses', stage/'share/licenses/edgeecho')
    # Preserve exact distro copyright notices for every copied dependency.
    copyright_files = set()
    for source in libraries.values():
        candidates = [source.resolve(), source]
        for candidate in candidates:
            lookup = subprocess.run(['dpkg-query', '-S', str(candidate)], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if lookup.returncode:
                continue
            for line in lookup.stdout.splitlines():
                package = line.split(': ', 1)[0].split(':')[0]
                notice = Path('/usr/share/doc')/package/'copyright'
                if notice.is_file():
                    copyright_files.add((package,notice))
            break
    for package, notice in copyright_files:
        shutil.copy2(notice, stage/'share/licenses'/f'{package}-copyright.txt')
    common = Path('/usr/share/common-licenses')
    if common.is_dir():
        shutil.copytree(common, stage/'share/licenses/common', symlinks=False)
    info = {
        'version':version_text, 'semantic_version':expected_identity[0], 'build_id':expected_identity[1], 'platform':platform.platform(),
        'glibc':platform.libc_ver(), 'bundle_kind':'host-ABI Linux runtime bundle',
        'included_libraries':sorted(libraries), 'system_abi_libraries':omitted,
        'platform_plugins':['offscreen','xcb','minimal'],
        'scope':'Requires the host glibc baseline or compatible newer distribution, system graphics drivers, fonts and an X11 session for xcb. No older-glibc portability claim.',
        'qt_source':'https://download.qt.io/archive/qt/',
        'source_note':'Exact distro patches/source correspond to the package versions in the included copyright notices; Ubuntu qt6-base source: https://launchpad.net/ubuntu/+source/qt6-base . Dynamic libraries may be replaced with ABI-compatible builds.',
    }
    (stage/'PACKAGE.json').write_text(json.dumps(info,indent=2)+'\n')
    digest_lines=[]
    for path in sorted(stage.rglob('*')):
        if path.is_file():
            digest_lines.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(stage).as_posix()}')
    (stage/'SHA256SUMS').write_text('\n'.join(digest_lines)+'\n')
    assert_sources_unchanged(source_before, fingerprints(ROOT))
    archive = output / (bundle_name + '.tar.gz')
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(stage, arcname=bundle_name)
    print(json.dumps({'directory':str(stage),'archive':str(archive),'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'libraries':len(libraries)},indent=2))

if __name__=='__main__':
    main()
