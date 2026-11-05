#!/usr/bin/env python3
"""Validate a relocated bundle without developer Qt lookup environment."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import re

from validation_support import assert_sources_unchanged, executable_identity, fingerprints, source_id, version

ROOT=Path(__file__).resolve().parents[1]
REPORT_PATH=None

def main():
    global REPORT_PATH
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('--package',type=Path,required=True)
    p.add_argument('--output',type=Path,default=ROOT/'build/package-validation')
    p.add_argument('--validation-report',type=Path,help='Require a passing current-source validation report')
    a=p.parse_args()
    output=a.output.resolve()
    if output==ROOT or not output.is_relative_to(ROOT):
        raise RuntimeError('Output must be a dedicated project-local directory')
    output.mkdir(parents=True,exist_ok=True)
    REPORT_PATH=output/'result.json'
    REPORT_PATH.write_text(json.dumps({'passed':False,'status':'running','platform':sys.platform})+'\n',encoding='utf-8')
    source_before=fingerprints(ROOT)
    expected_identity = (version(ROOT), source_id(source_before))
    if a.validation_report:
        validated = json.loads(a.validation_report.read_text(encoding='utf-8'))
        if not validated.get('passed') or not validated.get('source_unchanged') or validated.get('source_build_id') != expected_identity[1]:
            raise RuntimeError('Validation report is failing or does not describe current sources')
    relocated=output/'中文 空格目录'/'EdgeEcho'
    if relocated.exists():
        if not (relocated/'.edgeecho-package').is_file():raise RuntimeError('Refusing to replace unrelated relocation directory')
        shutil.rmtree(relocated)
    relocated.parent.mkdir(parents=True,exist_ok=True)
    shutil.copytree(a.package.resolve(),relocated)
    manifested=set()
    for line in (relocated/'SHA256SUMS').read_text(encoding='utf-8-sig').splitlines():
        digest,relative=line.split('  ',1)
        raw=relocated/relative
        path=raw.resolve()
        if relative in manifested or raw.is_symlink() or not path.is_relative_to(relocated.resolve()) or not path.is_file():
            raise RuntimeError('Invalid package manifest path: '+relative)
        if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise RuntimeError('Package manifest mismatch: '+relative)
        manifested.add(relative)
    actual={p.relative_to(relocated).as_posix() for p in relocated.rglob('*') if p.is_file() and p!=relocated/'SHA256SUMS'}
    if actual!=manifested:raise RuntimeError('Package contains missing or unmanifested files')
    package_info=json.loads((relocated/'PACKAGE.json').read_text(encoding='utf-8-sig'))
    if (package_info.get('semantic_version'),package_info.get('build_id')) != expected_identity:
        raise RuntimeError('Package manifest is stale or does not declare the current source build')
    env=dict(os.environ)
    removed=[]
    for key in list(env):
        if key.startswith(('QT_','QML_')) or key in ('QTDIR','LD_LIBRARY_PATH','LD_PRELOAD','CMAKE_PREFIX_PATH'):
            removed.append(key);env.pop(key,None)
    if os.name=='nt':
        winroot=env.get('SystemRoot',r'C:\Windows')
        env['PATH']=os.pathsep.join((str(relocated/'bin'),str(Path(winroot)/'System32'),winroot))
        cli=relocated/'bin/edgeecho_cli.exe';gui=relocated/'bin/edgeecho.exe'
    else:
        env['PATH']='/usr/bin:/bin'
        cli=relocated/'edgeecho_cli';gui=relocated/'edgeecho'
    for key,folder in (('XDG_CACHE_HOME','cache'),('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_RUNTIME_DIR','runtime')):
        value=output/folder;value.mkdir(exist_ok=True);value.chmod(0o700);env[key]=str(value)
    env['TMPDIR']=env['TMP']=env['TEMP']=str(output/'runtime')
    env['QT_QPA_PLATFORM']='offscreen'
    results={}
    def run(name,args,timeout=180,extra=None):
        current=dict(env)
        if extra:current.update(extra)
        r=subprocess.run([str(x) for x in args],env=current,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
        (output/(name+'.log')).write_text(r.stdout,encoding='utf-8')
        results[name]={'exit_code':r.returncode,'passed':r.returncode==0}
        if r.returncode:raise RuntimeError(f'{name} failed: {r.stdout}')
        return r.stdout
    version_text=run('version',[cli,'--version'])
    gui_version=run('gui_version',[gui,'--version'])
    if executable_identity(version_text)!=expected_identity or executable_identity(gui_version)!=expected_identity:
        raise RuntimeError('Packaged GUI/CLI identities differ from current source')
    db=output/'离线 演示.db'
    if db.exists():db.unlink()
    run('create_demo',[cli,'demo',db,'--records','2000'])
    before=hashlib.sha256(db.read_bytes()).hexdigest()
    run('inspect',[cli,'inspect',db])
    output_text=run('gui_smoke',[gui,'--smoke',db,'--screenshot',output/'relocated-gui.png'])
    if 'GUI smoke passed' not in output_text:raise RuntimeError('GUI smoke did not complete')
    if before!=hashlib.sha256(db.read_bytes()).hexdigest():raise RuntimeError('Source database changed')
    if os.name!='nt':
        # Check the relocated ELF resolves only package libs and system ABI libs.
        package_env=dict(env);package_env['LD_LIBRARY_PATH']=str(relocated/'lib')
        lines=[]
        for elf in [relocated/'bin/edgeecho',relocated/'bin/edgeecho_cli',*(relocated/'plugins/platforms').glob('*.so'),*(relocated/'lib').glob('*')]:
            if not elf.is_file():continue
            r=subprocess.run(['ldd',str(elf)],env=package_env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True)
            if 'not found' in r.stdout or '/.deps/' in r.stdout:raise RuntimeError('Unresolved/developer runtime dependency: '+r.stdout)
            lines.append(str(elf)+'\n'+r.stdout)
            dynamic=subprocess.run(['readelf','-d',str(elf)],text=True,stdout=subprocess.PIPE,check=True).stdout
            for line in dynamic.splitlines():
                if 'RPATH' not in line and 'RUNPATH' not in line:continue
                match=re.search(r'\[(.*?)\]',line)
                if not match or any(not part.startswith('$ORIGIN') for part in match[1].split(':')):
                    raise RuntimeError('Non-relative/developer RPATH survives: '+str(elf)+' '+line)
        (output/'dependencies.log').write_text('\n'.join(lines),encoding='utf-8')
    assert_sources_unchanged(source_before,fingerprints(ROOT))
    report={'passed':True,'source_unchanged':True,'platform':sys.platform,'version':version_text.strip(),'build_id':expected_identity[1],'gui_identity_verified':True,'relocated_package':str(relocated),
            'removed_environment':removed,'source_sha256_unchanged':before,'checks':results,
            'scope':'Relocated bundle on current host; not a clean OS image or an older ABI compatibility guarantee'}
    (output/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False,indent=2))

if __name__=='__main__':
    try:
        main()
    except Exception as error:
        if REPORT_PATH is not None:
            REPORT_PATH.write_text(json.dumps({'passed':False,'status':'failed','platform':sys.platform,'error':str(error)},indent=2)+'\n',encoding='utf-8')
        raise
