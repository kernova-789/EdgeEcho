#!/usr/bin/env python3
"""Build and validate the current source with fresh CTest JUnit evidence.

Default: native desktop Release and core ASan/UBSan. All outputs stay inside
EdgeEcho. --report explicitly publishes a selected report; by default only a
unique build/validation-current/run-*/report.json is written. Windows cross
compilation is build/ABI evidence and never counted as Windows runtime tests.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time
import uuid

from validation_support import (assert_sources_unchanged, assert_test_inventory, binary_format,
                                executable_identity, fingerprints, junit_result,
                                source_id, version)

ROOT = Path(__file__).resolve().parents[1]


def local_path(path: Path) -> Path:
    resolved = path.resolve()
    if resolved == ROOT or not resolved.is_relative_to(ROOT):
        raise RuntimeError('Validation paths must be dedicated locations inside EdgeEcho')
    return resolved


def executable(build: Path, name: str, config: str, suffix: str = '') -> Path:
    for path in (build/config/(name+suffix), build/(name+suffix)):
        if path.is_file():
            return path
    raise RuntimeError(f'Missing executable {name}{suffix} in {build}')


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--profiles', nargs='+', choices=('release','asan','windows-cross'),
                        default=['release'] if os.name == 'nt' else ['release','asan'])
    parser.add_argument('--release-build', type=Path, default=ROOT/'build')
    parser.add_argument('--asan-build', type=Path, default=ROOT/'build-asan')
    parser.add_argument('--windows-build', type=Path, default=ROOT/'build-windows')
    parser.add_argument('--output', type=Path, default=ROOT/'build/validation-current')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--parallel', type=int, default=2)
    parser.add_argument('--command-timeout', type=int, default=1200)
    parser.add_argument('--cmake-arg', action='append', default=[])
    args = parser.parse_args()
    if args.parallel < 1 or args.command_timeout < 1 or len(set(args.profiles)) != len(args.profiles):
        parser.error('Positive limits and unique profiles are required')
    output = local_path(args.output)
    run_dir = output/('run-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+uuid.uuid4().hex[:8])
    run_dir.mkdir(parents=True, exist_ok=False)
    report_path = local_path(args.report) if args.report else run_dir/'report.json'
    env = dict(os.environ)
    home = run_dir/'runtime'
    home.mkdir()
    home.chmod(0o700)
    for key, relative in (('XDG_CACHE_HOME','cache'),('XDG_CONFIG_HOME','config'),
                          ('XDG_DATA_HOME','data'),('XDG_RUNTIME_DIR','run')):
        folder=home/relative; folder.mkdir(); folder.chmod(0o700); env[key]=str(folder)
    env['TMPDIR']=env['TMP']=env['TEMP']=str(home)
    env['QT_QPA_PLATFORM']='offscreen'
    qt = ROOT/'.deps/qt/usr'
    qt_lib = qt/'lib/x86_64-linux-gnu'
    if os.name != 'nt' and qt_lib.is_dir():
        env['LD_LIBRARY_PATH']=str(qt_lib)+(os.pathsep+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    started = datetime.now(timezone.utc).isoformat()
    before = fingerprints(ROOT)
    expected_build = source_id(before)
    expected_version = version(ROOT)
    report = {'started_at_utc':started, 'scope':'Current-source validation; each profile records its own execution boundary',
              'source_build_id':expected_build, 'version':expected_version,
              'host':{'platform':platform.platform(),'machine':platform.machine()},
              'source_sha256':before, 'profiles':{}, 'passed':False, 'run_directory':str(run_dir)}
    active = None

    def run(command, folder: Path, label: str, *, current_env=None, capture=False):
        target=folder/(label+'.log')
        began=time.monotonic()
        try:
            completed=subprocess.run([str(x) for x in command], env=current_env or env, cwd=ROOT,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     timeout=args.command_timeout, check=False)
        except subprocess.TimeoutExpired as error:
            target.write_bytes(error.stdout or b'')
            if active is not None:
                active.setdefault('commands',[]).append({'command':[str(x) for x in command],
                    'exit_code':None,'timed_out':True,'elapsed_seconds':time.monotonic()-began,'log':str(target)})
            raise RuntimeError(f'{label} exceeded the command time limit; see {target}') from error
        target.write_bytes(completed.stdout)
        entry={'command':[str(x) for x in command], 'exit_code':completed.returncode,
               'elapsed_seconds':time.monotonic()-began, 'log':str(target)}
        if active is not None:
            active.setdefault('commands',[]).append(entry)
        if completed.returncode:
            raise RuntimeError(f'{label} failed ({completed.returncode}); see {target}')
        return completed.stdout.decode('utf-8',errors='replace') if capture else None

    def configure_args(build: Path, gui: bool, sanitizer: bool, config: str):
        command=['cmake','-S',ROOT,'-B',build,'-DBUILD_TESTING=ON',f'-DCMAKE_BUILD_TYPE={config}',
                 '-DEDGEECHO_BUILD_GUI='+('ON' if gui else 'OFF'),
                 '-DEDGEECHO_SANITIZERS='+('ON' if sanitizer else 'OFF')]
        if gui and os.name!='nt' and (qt_lib/'cmake/Qt6').is_dir():
            command += [f'-DCMAKE_PREFIX_PATH={qt}',f'-DCMAKE_LIBRARY_PATH={qt_lib}',f'-DCMAKE_INCLUDE_PATH={qt}/include']
        return [*command,*args.cmake_arg]

    try:
        for profile in args.profiles:
            folder=run_dir/profile;folder.mkdir()
            active={'passed':False};report['profiles'][profile]=active
            if profile=='windows-cross':
                if os.name=='nt':
                    raise RuntimeError('windows-cross is a Linux-host build check; use native release on Windows')
                build=local_path(args.windows_build)
                cross_env=dict(env)
                mingw=ROOT/'.deps/mingw'
                if mingw.is_dir():
                    cross_env['EDGEECHO_MINGW_ROOT']=str(mingw)
                    cross_env['PATH']=str(mingw/'usr/bin')+os.pathsep+cross_env.get('PATH','')
                command=configure_args(build,False,False,'Release')
                command += [f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/mingw-x86_64.cmake']
                run(command,folder,'configure',current_env=cross_env)
                run(['cmake','--build',build,'--config','Release','--parallel',args.parallel],folder,'build',current_env=cross_env)
                bins={}
                for name in ('edgeecho_cli','database_tests','codec_playback_tests','session_tests','export_tests','browsing_tests'):
                    path=executable(build,name,'Release','.exe')
                    abi=binary_format(path)
                    if abi['format']!='PE' or abi['machine']!=0x8664:
                        raise RuntimeError('Cross output is not Windows x86-64 PE: '+str(path))
                    bins[name]={'path':str(path),'sha256':sha(path),'abi':abi}
                cli=Path(bins['edgeecho_cli']['path'])
                if expected_build.encode() not in cli.read_bytes():
                    raise RuntimeError('Cross CLI does not embed the current source build identity')
                active.update({'passed':True,'artifacts':bins,'runtime_executed':False,
                               'scope':'Windows x86-64 cross compilation/PE inspection only; no Windows execution'})
            else:
                gui=profile=='release'; sanitizer=profile=='asan'
                config='Debug' if sanitizer else 'Release'
                build=local_path(args.asan_build if sanitizer else args.release_build)
                profile_env=dict(env)
                if sanitizer:
                    profile_env['ASAN_OPTIONS']='detect_leaks=1'
                    profile_env['UBSAN_OPTIONS']='print_stacktrace=1:halt_on_error=1'
                run(configure_args(build,gui,sanitizer,config),folder,'configure',current_env=profile_env)
                run(['cmake','--build',build,'--config',config,'--parallel',args.parallel],folder,'build',current_env=profile_env)
                inventory=json.loads(run(['ctest','--test-dir',build,'-C',config,'--show-only=json-v1'],folder,'inventory',current_env=profile_env,capture=True))
                names=[test['name'] for test in inventory.get('tests',[])]
                assert_test_inventory(names,gui=gui)
                junit=folder/'ctest-junit.xml'
                if junit.exists():
                    raise RuntimeError('Fresh JUnit path already exists before execution')
                test_started=time.time_ns()
                run(['ctest','--test-dir',build,'-C',config,'--output-on-failure','--output-junit',junit],folder,'ctest',current_env=profile_env)
                tests=junit_result(junit,names,started_ns=test_started)
                identities={};artifacts={}
                for name in (('edgeecho_cli','edgeecho') if gui else ('edgeecho_cli',)):
                    path=executable(build,name,config,'.exe' if os.name=='nt' else '')
                    identity=executable_identity(run([path,'--version'],folder,name+'-version',current_env=profile_env,capture=True))
                    if identity!=(expected_version,expected_build):
                        raise RuntimeError(f'{name} build identity differs from current source')
                    identities[name]={'version':identity[0],'build_id':identity[1]}
                    artifacts[name]={'path':str(path),'sha256':sha(path),'abi':binary_format(path)}
                active.update({'passed':True,'tests':tests,'identities':identities,'artifacts':artifacts,
                               'runtime_executed':True,'scope':'Native '+platform.system()+' '+config+(' GUI/core/CLI' if gui else ' ASan+UBSan core/CLI'),
                               'junit':str(junit)})
            assert_sources_unchanged(before,fingerprints(ROOT))
        report['passed']=True
    except Exception as error:
        report['error']=str(error)
        if active is not None:
            active['error']=str(error)
    finally:
        try:
            after=fingerprints(ROOT)
            assert_sources_unchanged(before,after)
            report['source_unchanged']=True
        except Exception as error:
            report['source_unchanged']=False; report['passed']=False; report['source_error']=str(error)
        report['finished_at_utc']=datetime.now(timezone.utc).isoformat()
        (run_dir/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        report_path.parent.mkdir(parents=True,exist_ok=True)
        if report_path!=run_dir/'report.json':
            report_path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    summary={'passed':report['passed'],'build_id':expected_build,'report':str(report_path),
             'profiles':{name:{'passed':entry.get('passed',False),'tests_passed':entry.get('tests',{}).get('passed'),
                               'runtime_executed':entry.get('runtime_executed',False)} for name,entry in report['profiles'].items()}}
    if report.get('error'):summary['error']=report['error']
    print(json.dumps(summary,ensure_ascii=False,indent=2))
    return 0 if report['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
