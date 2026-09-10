#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""NextOS: bounded read-only Android inventory; no guest code execution."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT=Path(__file__).resolve().parents[1]
LIMIT=512*1024*1024
ABI_MACHINE={'arm64-v8a':183,'armeabi-v7a':40,'x86':3,'x86_64':62}
def digest(data):return hashlib.sha256(data).hexdigest()

def member_index(archive):
    seen=set();total=0
    for info in archive.infolist():
        name=info.filename;parts=PurePosixPath(name).parts
        if (not name or name.startswith('/') or '\\' in name or '..' in parts
                or any(ord(c)<32 for c in name) or name.casefold() in seen):
            raise ValueError('unsafe or duplicate archive member')
        if stat.S_ISLNK(info.external_attr>>16):raise ValueError('symlink archive member')
        if info.file_size>LIMIT:raise ValueError('member exceeds 512 MiB inventory limit')
        if info.flag_bits&1:raise ValueError('encrypted member is unsupported')
        total+=info.file_size
        if total>8*1024**3:raise ValueError('archive exceeds 8 GiB inventory budget')
        seen.add(name.casefold())
    if len(seen)>100000:raise ValueError('too many archive members')
    return archive.namelist()

def manifest_identity(data):
    if len(data)>2*1024*1024:raise ValueError('manifest exceeds inventory limit')
    if b'<!DOCTYPE' in data.upper() or b'<!ENTITY' in data.upper():
        raise ValueError('manifest entities are unsupported')
    spec=importlib.util.spec_from_file_location('nextos_inventory_nxe',ROOT/'suportando_outros_devices/extrator-universal/nxextract.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    package,split=module.parse_android_manifest(data)
    result={'package_id':package,'split':split,'version_name':None,'version_code':None}
    if data.lstrip().startswith(b'<'):
        element=ET.fromstring(data);ns='{http://schemas.android.com/apk/res/android}'
        result.update(version_name=element.get(ns+'versionName'),version_code=element.get(ns+'versionCode'))
    return result

def parse_symbols(text):
    symbols={};unsupported=set()
    for line in text.splitlines():
        fields=line.split(None,7)
        if len(fields)!=8 or not fields[0].endswith(':'):continue
        _,value,size,kind,binding,visibility,index,name=fields
        if kind=='TLS':unsupported.add('TLS symbol')
        if kind=='IFUNC':unsupported.add('IFUNC symbol')
        if index=='UND' and name and name!='0':
            name=name.split()[0]
            symbols[(name,kind,binding)]={'name':name,'kind':kind,'binding':binding,'version':name.partition('@')[2] or None}
    return list(symbols.values()),sorted(unsupported)

def library_inventory(data,name,temp):
    if data[:4]!=b'\x7fELF' or len(data)<64 or data[5]!=1:
        raise ValueError('native member is not a supported little-endian ELF')
    machine=int.from_bytes(data[18:20],'little');abi=name.split('/')[1]
    path=temp/'guest.so';path.write_bytes(data);path.chmod(0o600)
    env=dict(os.environ,LC_ALL='C')
    def read(*flags):return subprocess.check_output(['readelf',*flags,str(path)],stderr=subprocess.DEVNULL,text=True,env=env,timeout=45)
    dynamic=read('-dW');imports,unsupported=parse_symbols(read('--dyn-syms','--wide'))
    relocation=read('-rW')
    for marker in ('RELR','ANDROID_REL','ANDROID_RELA','TLSDESC','IRELATIVE','TLS_','0x6000000f','0x60000011'):
        if marker in dynamic or marker in relocation:unsupported.append(marker)
    if machine!=ABI_MACHINE.get(abi):unsupported.append('directory/ELF ABI mismatch')
    return {'member':name,'size':len(data),'sha256':digest(data),'elf_class':data[4]*32,
            'machine':machine,'abi':abi,'needed':re.findall(r'\(NEEDED\).*?\[(.*?)\]',dynamic),
            'imports':imports[:4096],'import_count':len(imports),'imports_truncated':len(imports)>4096,
            'v5_preflight_blockers':sorted(set(unsupported)),
            'scope':'static hints; runtime dlsym/JNI calls and complete relocation support still require investigation'}

def inventory(paths,scratch):
    scratch.mkdir(parents=True,exist_ok=True)
    reports=[]
    for number,path in enumerate(paths,1):
        path=Path(path)
        h=hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
        with zipfile.ZipFile(str(path)) as apk:
            names=member_index(apk)
            if 'AndroidManifest.xml' not in names:
                raise ValueError('supply complete base/split APKs separately; nested APKM/APKS/XAPK are not expanded automatically')
            identity=manifest_identity(apk.read('AndroidManifest.xml'))
            if not identity['package_id']:raise ValueError('manifest package identity missing')
            if identity['version_name'] is None and shutil.which('aapt'):
                result=subprocess.run(['aapt','dump','badging',str(path)],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,timeout=30)
                if result.returncode==0:
                    line=next((x for x in result.stdout.splitlines() if x.startswith('package:')), '')
                    values=dict(re.findall(r"(\w+)='([^']*)'",line))
                    if values.get('name')==identity['package_id']:
                        identity.update(version_name=values.get('versionName'),version_code=values.get('versionCode'))
            libraries=[]
            with tempfile.TemporaryDirectory(prefix='private-inventory-',dir=str(scratch)) as directory:
                for name in names:
                    parts=PurePosixPath(name).parts
                    if len(parts)==3 and parts[0]=='lib' and name.endswith('.so'):
                        libraries.append(library_inventory(apk.read(name),name,Path(directory)))
            hints=[]
            for token,engine in [('libunity.so','Unity'),('libil2cpp.so','Unity IL2CPP'),('libmonodroid.so','Mono Android'),('libgodot','Godot'),('libcocos','Cocos family')]:
                if any(token in name for name in names):hints.append(engine)
            reports.append({'input_id':'container-%02d'%number,'container_size':path.stat().st_size,
                'container_sha256':h.hexdigest(),**identity,'engine_hints':hints,
                'preferred_abi':'arm64-v8a' if any(x['abi']=='arm64-v8a' for x in libraries) else None,
                'libraries':libraries})
    packages={r['package_id'] for r in reports}
    if len(packages)!=1:raise ValueError('input APKs belong to different packages')
    return {'schema':'nextos-android-inventory-v1','containers':reports,
            'scope':'static inventory; no viability, complete split set or gameplay certification',
            'privacy':'no input filenames or host paths; review technical metadata before publishing'}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('apk',nargs='+')
    p.add_argument('--output');p.add_argument('--scratch',default=str(ROOT/'work/inventory-private'))
    a=p.parse_args()
    try:
        data=json.dumps(inventory(a.apk,Path(a.scratch)),indent=2)+'\n'
        if a.output:
            fd=os.open(a.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
            with os.fdopen(fd,'w',encoding='utf-8') as out:out.write(data)
        else:print(data,end='')
    except (OSError,ValueError,ET.ParseError,zipfile.BadZipFile,subprocess.SubprocessError) as error:
        print('INVENTORY ERROR: '+str(error),file=sys.stderr);sys.exit(1)
