#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build and exercise the original host shader lesson. Downloads require --fetch."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
HERE=Path(__file__).resolve().parent
def run(args,success=True):
    print('+ '+' '.join(map(str,args)),flush=True)
    result=subprocess.run(list(map(str,args)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    print(result.stdout,flush=True)
    if (result.returncode==0)!=success:raise ValueError('unexpected command result')
    return result.stdout
def main(deps,output,fetch):
    lock=json.loads((HERE/'sources.json').read_text())
    if output.exists():raise ValueError('output directory must be new')
    for tool in ('git','g++','glslangValidator'):
        if not shutil.which(tool):raise ValueError('missing host tool: '+tool)
    for name in ('SPIRV-Cross','smol-v'):
        info=lock[name];path=deps/name
        if not path.exists():
            if not fetch:raise ValueError('dependency absent; prepare sources or pass --fetch explicitly')
            deps.mkdir(parents=True,exist_ok=True)
            run(['git','clone','--filter=blob:none','--no-checkout',info['repository'],path])
            run(['git','-C',path,'checkout','--detach',info['commit']])
        commit=subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True).strip()
        if commit!=info['commit']:raise ValueError(name+' checkout has wrong commit')
        if subprocess.check_output(['git','-C',str(path),'status','--porcelain'],text=True):raise ValueError(name+' checkout is dirty')
        for relative,expected in info.get('files',{}).items():
            if hashlib.sha256((path/relative).read_bytes()).hexdigest()!=expected:raise ValueError('dependency hash mismatch')
    output.mkdir(parents=True)
    cross=deps/'SPIRV-Cross';smolv=deps/'smol-v/source';exe=output/'translate'
    run(['g++','-std=c++17','-O2','-I'+str(cross),'-I'+str(smolv),HERE/'translate.cpp',smolv/'smolv.cpp',
         *[cross/f for f in ('spirv_cfg.cpp','spirv_cross.cpp','spirv_cross_parsed_ir.cpp','spirv_parser.cpp','spirv_glsl.cpp')],'-o',exe])
    for stage in ('vert','frag'):
        spv=output/('color.'+stage+'.spv');glsl=output/('color.'+stage)
        run(['glslangValidator','-V','--target-env','vulkan1.0',HERE/('color.'+stage),'-o',spv])
        run([exe,spv,glsl]);run(['glslangValidator','-S',stage,glsl])
    spv=output/'unsupported.spv';rejected=output/'unsupported.glsl'
    run(['glslangValidator','-V',HERE/'unsupported.comp','-o',spv])
    message=run([exe,spv,rejected],False)
    if 'unsupported execution model' not in message or rejected.exists():raise ValueError('negative translation did not reject cleanly')
    report={'status':'PASS','checks':['vertex-translation','fragment-translation','lossless-smolv','essl100-validation','compute-rejected'],
        'glslang_version':subprocess.check_output(['glslangValidator','--version'],text=True).splitlines()[0],
        'source_pins':lock,'physical_support_proven':False,'scope':'original host shader lesson; not FP2 conversion certification'}
    (output/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--deps',required=True);p.add_argument('--output',required=True);p.add_argument('--fetch',action='store_true');a=p.parse_args()
    try:main(Path(a.deps).resolve(),Path(a.output).resolve(),a.fetch)
    except (OSError,ValueError,subprocess.SubprocessError) as e:sys.exit('SHADER LAB ERROR: '+str(e))
