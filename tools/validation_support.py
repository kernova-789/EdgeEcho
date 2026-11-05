"""Shared source identity and strict fresh-result checks for validation/packaging."""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import xml.etree.ElementTree as ET

INPUTS = {
    'include': ('*.hpp',), 'src': ('*.cpp', '*.hpp', '*.in'),
    'app': ('*.cpp', '*.hpp'), 'tools': ('*.cpp', '*.py', '*.sh', '*.ps1'),
    'tests': ('*.cpp', '*.hpp', '*.py', '*.sql', '*.json'),
    'cmake': ('*.cmake',), '.github': ('*.yml', '*.yaml'),
}


def fingerprints(root: Path) -> dict[str, str]:
    files = {root/'CMakeLists.txt', root/'third_party/sqlite/sqlite3.c', root/'third_party/sqlite/sqlite3.h'}
    for folder, patterns in INPUTS.items():
        for pattern in patterns:
            files.update((root/folder).rglob(pattern))
    result = {}
    for path in sorted(files):
        if not path.is_file():
            raise RuntimeError(f'Missing source input: {path}')
        result[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def source_id(inputs: dict[str, str]) -> str:
    if not inputs:
        raise RuntimeError('Empty source fingerprint set')
    content = ''.join(f'{path}:{inputs[path]}\n' for path in sorted(inputs))
    return 'source-sha256:' + hashlib.sha256(content.encode('utf-8')).hexdigest()


def version(root: Path) -> str:
    match = re.search(r'project\(EdgeEcho VERSION ([0-9]+\.[0-9]+\.[0-9]+)',
                      (root/'CMakeLists.txt').read_text(encoding='utf-8'))
    if not match:
        raise RuntimeError('CMake project version missing')
    return match[1]


def executable_identity(text: str) -> tuple[str, str]:
    match = re.search(r'EdgeEcho\s+([0-9]+\.[0-9]+\.[0-9]+)', text)
    build = re.search(r'source-sha256:[a-f0-9]{64}', text)
    if not match or not build:
        raise RuntimeError('Executable omitted version/build identity')
    return match[1], build[0]


def assert_sources_unchanged(before: dict[str, str], after: dict[str, str]) -> None:
    if not before or before != after:
        changed = sorted(name for name in set(before) | set(after) if before.get(name) != after.get(name))
        raise RuntimeError('Source inputs changed during validation: ' + ', '.join(changed[:12]))


def assert_test_inventory(names: list[str], *, gui: bool) -> None:
    required = {'database','codec_playback','session','export','browsing','validation_runner',
                'cli_demo','cli_inspect','cli_invalid_number'}
    if gui:
        required.update(('gui_smoke','gui_inputs','plot_widget','time_range_editor'))
    if len(set(names)) != len(names) or not required.issubset(names):
        raise RuntimeError('Configured build omitted required current-version tests: ' +
                           ', '.join(sorted(required-set(names))))


def junit_result(path: Path, expected_names: list[str], *, started_ns: int) -> dict:
    if not path.is_file() or path.stat().st_mtime_ns + 2_000_000_000 < started_ns:
        raise RuntimeError('Missing or stale JUnit result; older test logs are not evidence')
    cases = ET.parse(path).getroot().findall('.//testcase')
    if not cases or not expected_names:
        raise RuntimeError('An empty test run cannot pass validation')
    names = [item.get('name', '') for item in cases]
    if len(set(names)) != len(names) or sorted(names) != sorted(expected_names):
        raise RuntimeError('Executed test set does not match the freshly configured CTest inventory')
    bad = [item.get('name') for item in cases if item.find('failure') is not None or
           item.find('error') is not None or item.find('skipped') is not None or
           item.get('status', '').lower() in ('notrun', 'disabled', 'skipped', 'fail', 'failed')]
    if bad:
        raise RuntimeError('Failed/skipped/disabled tests are not a complete pass: ' + ', '.join(bad))
    return {'passed': len(cases), 'failed': 0, 'skipped': 0, 'tests': names}


def binary_format(path: Path) -> dict:
    data = path.read_bytes()
    if data.startswith(b'\x7fELF') and len(data) >= 20:
        endian = 'little' if data[5] == 1 else 'big'
        machine = int.from_bytes(data[18:20], endian)
        return {'format': 'ELF', 'bits': 64 if data[4] == 2 else 32, 'machine': machine}
    if data.startswith(b'MZ') and len(data) > 64:
        offset = int.from_bytes(data[60:64], 'little')
        if data[offset:offset+4] == b'PE\0\0':
            machine = int.from_bytes(data[offset+4:offset+6], 'little')
            return {'format': 'PE', 'machine': machine, 'bits': 64 if machine in (0x8664, 0xaa64) else 32}
    raise RuntimeError(f'Unrecognized executable ABI: {path}')
