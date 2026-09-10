#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise real NXExtract transactions and the real ARM64 guest under QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def execute(args,log,success=True,contains=None):
    print('+ '+' '.join(map(str,args)),flush=True)
    result=subprocess.run(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
    log.write_text(result.stdout)
    print(result.stdout[-1800:],flush=True)
    if (result.returncode==0)!=success or (contains and contains not in result.stdout):
        raise AssertionError('unexpected command result; see '+str(log))
    return result

def main(output):
    area=output/'pipeline-tests'
    if area.exists():raise ValueError('test area exists; preserve evidence and choose a new build directory')
    area.mkdir()
    source=output/'framework-pin/tool-source'
    extractor=source/'suportando_outros_devices/extrator-universal/nxextract.py'
    original=output/'training-input.apk';base=output/'runtime'
    def install(case,apk,success=True,extra=()):
        game=area/case;shutil.copytree(str(base),str(game))
        execute([sys.executable,extractor,'install','--recipe',game/'extractor.json',
                 '--game-dir',game,'--input',apk,'--ui','none','--abi','arm64-v8a',*extra],area/(case+'.log'),success)
        if success:
            assert (game/'seed.txt').read_bytes()==b'7\n'
            assert sha(game/'lib/arm64-v8a/libtraining.so')==sha(output/'libtraining.so')
            assert (game/'.nxextract-first-port.json').is_file()
        else:assert not (game/'.nxextract-first-port.json').exists()
        return game
    good=install('clean',original)
    execute([sys.executable,extractor,'verify','--recipe',good/'extractor.json','--game-dir',good],area/'verify.log')
    prefix=['qemu-aarch64','-L','/usr/aarch64-linux-gnu','-E','LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu']
    execute(prefix+[good/'bin/aarch64/first-port-nextos',good,'--self-test'],area/'cpu.log',contains='PASS: Android guest executed')
    failed=execute(prefix+[good/'bin/aarch64/first-port-nextos',good,'--omit-log'],area/'missing-import.log',False,'FAIL resolve')
    assert 'guest[4]' not in failed.stdout,'constructors ran before mandatory import resolution'
    with zipfile.ZipFile(str(original)) as z:members={n:z.read(n) for n in z.namelist()}
    def fixture(name,entries):
        path=area/(name+'.apk')
        with zipfile.ZipFile(str(path),'x',compression=zipfile.ZIP_DEFLATED) as z:
            for n in sorted(entries,reverse=True):z.writestr(n,entries[n])
        return path
    repacked=fixture('repacked',dict(members,**{'META-INF/teaching.txt':b'packaging difference'}))
    assert sha(repacked)!=sha(original)
    install('compatible-repacked',repacked)
    wrong=dict(members);wrong['AndroidManifest.xml']=wrong['AndroidManifest.xml'].replace(b'org.nextos.training',b'org.nextos.other')
    install('wrong-package',fixture('other',wrong),False)
    wrong=dict(members);wrong['assets/seed.txt']=b'seed=8\n'
    install('wrong-payload',fixture('corrupt',wrong),False)
    wrong=dict(members);del wrong['assets/seed.txt']
    install('missing-payload',fixture('incomplete',wrong),False)
    wrong=dict(members);blob=bytearray(wrong['lib/arm64-v8a/libtraining.so']);blob[18:20]=(40).to_bytes(2,'little');wrong['lib/arm64-v8a/libtraining.so']=bytes(blob)
    install('wrong-abi',fixture('arm32-header',wrong),False)
    tracked=['seed.txt','lib/arm64-v8a/libtraining.so','.nxextract-first-port.json']
    before={name:sha(good/name) for name in tracked}
    recipe=json.loads((good/'extractor.json').read_text())
    recipe['hooks'][0]['argv']=['python3','-c','raise SystemExit(19)']
    bad_recipe=good/'failed-hook.json';bad_recipe.write_text(json.dumps(recipe))
    execute([sys.executable,extractor,'install','--recipe',bad_recipe,'--game-dir',good,
             '--input',original,'--force-source','--ui','none','--abi','arm64-v8a'],area/'hook-rollback.log',False)
    assert {name:sha(good/name) for name in tracked}==before,'failed hook modified committed data/marker'
    report={'schema':1,'status':'PASS','checks':['clean-extraction','hook-output','installed-payload-verification',
        'arm64-guest-execution','missing-import-before-constructors','compatible-repacking',
        'wrong-package-rejected','wrong-payload-rejected','missing-payload-rejected','wrong-abi-rejected','hook-rollback'],
        'linux_executable_sha256':sha(output/'first-port-nextos'),'guest_sha256':sha(output/'libtraining.so'),
        'input_sha256':sha(original),'scope':'host extraction without UI and CPU emulation',
        'physical_support_proven':False,'graphical_ui_proven':False}
    (area/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: '+str(len(report['checks']))+' integrated checks; physical/UI approval remains untested',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('build_directory');a=p.parse_args()
    try:main(Path(a.build_directory).resolve())
    except (OSError,ValueError,AssertionError,subprocess.SubprocessError) as e:sys.exit('PIPELINE TEST ERROR: '+str(e))
