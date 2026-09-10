#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the original ARM64 teaching port in the public SDK, without packaging a release."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
def run(*args):
    print('+ '+' '.join(map(str,args)),flush=True)
    subprocess.run(list(map(str,args)),check=True,cwd=str(ROOT))
def sha(data): return hashlib.sha256(data).hexdigest()
def write_json(path,value): path.write_text(json.dumps(value,indent=2)+'\n')

def build(output):
    if output.exists(): raise ValueError('output must be new; choose another development directory')
    for tool in ('cmake','clang','ld.lld','aarch64-linux-gnu-gcc','readelf'):
        if not shutil.which(tool): raise ValueError('missing tool: '+tool+'; use the public SDK')
    output.mkdir(parents=True)
    run(sys.executable,ROOT/'tools/pin_collection.py','--destination',output/'framework-pin')
    source=output/'framework-pin/source'
    guest=output/'libtraining.so'
    run('clang','--target=aarch64-linux-android21','-fuse-ld=lld','-nostdlib',
        '-shared','-fPIC','-ffreestanding','-fno-stack-protector','-fno-builtin','-O2',
        '-Wl,--hash-style=both','-Wl,--pack-dyn-relocs=none','-Wl,-soname,libtraining.so',
        HERE/'guest.c','-o',guest)
    run('cmake','-S',source/'framework/nxloader','-B',output/'loader-build',
        '-DCMAKE_SYSTEM_NAME=Linux','-DCMAKE_SYSTEM_PROCESSOR=aarch64',
        '-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc','-DCMAKE_BUILD_TYPE=Release',
        '-DNXLOADER_BUILD_TESTS=OFF','-DNXLOADER_BUILD_TOOLS=OFF')
    run('cmake','--build',output/'loader-build','--parallel','2')
    executable=output/'first-port-nextos'
    run('aarch64-linux-gnu-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-fno-omit-frame-pointer','-I'+str(source/'framework/nxloader/include'),
        '-I/usr/include/aarch64-linux-gnu','-idirafter','/usr/include',
        HERE/'adapter.c',output/'loader-build/libnxloader.a',
        '-L/usr/lib/aarch64-linux-gnu','-lSDL2','-lGLESv2','-lpthread','-lm','-ldl',
        '-o',executable)
    run(sys.executable,ROOT/'tools/audit_elf.py',executable)
    write_json(output/'COMPILED.json',compiled_identity(output))
    prepare(output)

def compiled_identity(output):
    return {'sources':{name:sha((HERE/name).read_bytes()) for name in ('guest.c','adapter.c','demo.h')},
            'guest_sha256':sha((output/'libtraining.so').read_bytes()),
            'linux_sha256':sha((output/'first-port-nextos').read_bytes()),
            'pin_sha256':sha((output/'framework-pin/FRAMEWORK-BUILD-PIN.json').read_bytes())}

def prepare(output):
    if json.loads((output/'COMPILED.json').read_text())!=compiled_identity(output):
        raise ValueError('compiled sources/binaries/pin changed; do not reuse these bytes')
    if (output/'generated').exists():raise ValueError('generated candidate already exists; preserve it')
    source=output/'framework-pin/tool-source';guest=output/'libtraining.so';executable=output/'first-port-nextos'
    input_path=output/'training-input.apk'
    manifest=b'<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.nextos.training" android:versionCode="1" android:versionName="1.0"><application/></manifest>'
    import io
    container=io.BytesIO()
    with zipfile.ZipFile(container,'w',compression=zipfile.ZIP_STORED) as apk:
        for name,data in [('AndroidManifest.xml',manifest),('lib/arm64-v8a/libtraining.so',guest.read_bytes()),('assets/seed.txt',b'seed=7\n')]:
            info=zipfile.ZipInfo(name,date_time=(2000,1,1,0,0,0));info.external_attr=0o100644<<16
            apk.writestr(info,data)
    input_bytes=container.getvalue()
    if input_path.exists():
        if input_path.read_bytes()!=input_bytes:raise ValueError('original input changed')
    else:input_path.write_bytes(input_bytes)
    recipe={
        'schema':1,'id':'first-port','version':'1','title':'NextOS Training / Treino',
        'abi_order':['arm64-v8a'],
        'input':{'search_dirs':['gamedata','.'],'prefer_first_nonempty':True,'sniff_all_in_primary':True,'packages':['org.nextos.training']},
        'extract':[
            {'id':'guest','source':{'kind':'entry','patterns':['lib/{abi}/libtraining.so']},
             'destination':'lib/{abi}/libtraining.so',
             'validate':{'type':'file','size':guest.stat().st_size,'sha256':sha(guest.read_bytes()),'elf_machine':'{abi}'}},
            {'id':'seed','source':{'kind':'entry','patterns':['assets/seed.txt']},'destination':'seed.txt',
             'source_validate':{'type':'file','size':7,'sha256':sha(b'seed=7\n')},
             'output_validate':{'type':'file','size':2,'sha256':sha(b'7\n')}}],
        'hooks':[{'id':'seed','transactional':True,'argv':['python3','{game_dir}/nxextract/prepare_seed.py'],
                  'checkpoint':[{'path':'seed.txt','type':'file','size':2,'sha256':sha(b'7\n')}]}],
        'validate':[{'path':'lib/{abi}/libtraining.so','type':'file','sha256':sha(guest.read_bytes())},
                    {'path':'seed.txt','type':'file','sha256':sha(b'7\n')}],
        'commit':['lib/{abi}/libtraining.so','seed.txt'],'marker':'.nxextract-first-port.json',
        'space':{'safety_bytes':1048576}}
    project=output/'project';project.mkdir(exist_ok=True)
    write_json(project/'extractor.json',recipe)
    shutil.copy2(HERE/'prepare_seed.py',project/'prepare_seed.py')
    shutil.copy2(ROOT/'LICENSE',project/'LICENSE')
    runtime=project/'runtime';(runtime/'bin/aarch64').mkdir(parents=True,exist_ok=True)
    records=[]
    def member(src,relative,role,mode):
        target=runtime/relative;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(str(src),str(target));target.chmod(mode)
        records.append({'role':role,'path':relative,'mode':'%04o'%mode,'sha256':sha(target.read_bytes())})
    member(executable,'bin/aarch64/first-port-nextos','executable',0o755)
    member(project/'extractor.json','extractor.json','nxextract-recipe',0o644)
    member(HERE/'prepare_seed.py','nxextract/prepare_seed.py','nxextract-helper',0o644)
    nxe=source/'suportando_outros_devices/extrator-universal'
    for name,role in [('nxextract.py','nxextract-engine'),('run-extractor.sh','nxextract-runner'),('nxextract-runtime-env.sh','nxextract-runtime-env')]:
        member(nxe/name,'nxextract/'+name,role,0o644)
    member(nxe/'ui/release/aarch64/nxextract-ui','nxextract/nxextract-ui','nxextract-ui',0o755)
    roles=('executable','private-library','runtime-data','runtime-hook','nxextract-recipe','nxextract-engine','nxextract-runner','nxextract-runtime-env','nxextract-ui','nxextract-helper','nxextract-spec','nxsplash')
    records.sort(key=lambda r:(roles.index(r['role']),r['path']))
    template=json.loads((source/'framework/nxgenerator/examples/nxproject-aarch64.example.json').read_text())
    template.update(schema_version=3,runtime_root='runtime')
    template['nxport'].update(schema_version=3,id='first-port',title='NextOS Training / Treino',launcher_name='NextOS Training.sh',executable='bin/aarch64/first-port-nextos',required_files=['bin/aarch64/first-port-nextos','nxextract/prepare_seed.py'],generation_runtime=records)
    template['nxextract_recipe']='runtime/extractor.json'
    template['documentation']={'status':'scaffold','proven_support':[]}
    template['portmaster']['min_glibc']='2.28'
    template['language_access']={'mode':'none','supported':[],'fallback':'','sinks':[]}
    template['controls']={'actions':[{'id':'demo.move','kind':'vector','sinks':['adapter.game.move']},{'id':'demo.exit','kind':'button','sinks':['adapter.game.exit']}],
                          'contexts':{'menu':{'LEFT_STICK':'demo.move','START':'demo.exit'},'gameplay':{'LEFT_STICK':'demo.move','START':'demo.exit'}}}
    template['graphics']={'uses_gl':True,'api':'gles','profile':'es','version':'2.0','version_policy':'minimum','shader_dialect':'essl100'}
    write_json(project/'nxproject.json',template)
    run(sys.executable,nxe/'nxextract.py','recipe-check','--recipe',project/'extractor.json')
    run(sys.executable,source/'framework/nxgenerator/nxgenerator.py',project/'nxproject.json',
        '--source-root',project,'--output',output/'generated')
    # Separate headless development directory; generated content remains untouched.
    dev=output/'runtime';(dev/'bin/aarch64').mkdir(parents=True)
    (dev/'nxextract').mkdir()
    for src,dest in [(executable,'bin/aarch64/first-port-nextos'),(project/'extractor.json','extractor.json'),(HERE/'prepare_seed.py','nxextract/prepare_seed.py'),(HERE/'adapter-contract.json','adapter-contract.json')]:
        shutil.copy2(src,dev/dest)
    metadata={'schema':1,'purpose':'original teaching input; not an installable Android app or a port release',
              'package_id':'org.nextos.training','game_version':'1.0','abi':'arm64-v8a',
              'input_size':input_path.stat().st_size,'input_sha256':sha(input_path.read_bytes()),
              'guest_sha256':sha(guest.read_bytes()),'linux_executable_sha256':sha(executable.read_bytes()),
              'physical_support_proven':False}
    write_json(output/'BUILD-RESULT.json',metadata)
    installation=(HERE/'INSTALLATION.md.in').read_text()
    for key in ('input_size','input_sha256'):installation=installation.replace('@'+key.upper()+'@',str(metadata[key]))
    (dev/'INSTALLATION.md').write_text(installation)
    print('BUILD COMPLETE: development runtime, original input and separate unchanged generator scaffold',flush=True)
    print('NEXT: python3 examples/first-port/test_pipeline.py '+str(output),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True);a=p.parse_args()
    try: build(Path(a.output).resolve())
    except (ValueError,OSError,subprocess.CalledProcessError) as e:sys.exit('BUILD ERROR: '+str(e))
