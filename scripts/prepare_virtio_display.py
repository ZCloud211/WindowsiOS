#!/usr/bin/env python3
"""Stage verified official ARM64 display drivers, preserving catalogued files."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import os

ROOT=Path(__file__).resolve().parent.parent
PACKAGE=ROOT/'build/verified-virtio-display-arm64'
BUNDLE=ROOT/'ios/Sources/Resources/DisplayDrivers'

def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def require(condition,message):
    if not condition:raise ValueError(message)

def main():
    hashes={}
    for line in (PACKAGE/'SHA256SUMS.txt').read_text().splitlines():
        digest,name=line.split('  ',1)
        require(name in {'SOURCE.json','viogpudo.sys','viogpudo.inf','viogpudo.cat'} and name not in hashes,'Unexpected file')
        hashes[name]=digest
    require(len(hashes)==4,'Incomplete display package')
    for name,digest in hashes.items():
        p=PACKAGE/name
        require(not p.is_symlink() and sha(p)==digest,name+' hash mismatch')
    data=(PACKAGE/'viogpudo.sys').read_bytes();pe=struct.unpack_from('<I',data,60)[0]
    require(data[:2]==b'MZ' and data[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',data,pe+4)[0]==0xaa64,'Driver is not Windows ARM64')
    require(struct.unpack_from('<H',data,pe+24+68)[0]==1,'Driver is not Native subsystem')
    source=json.loads((PACKAGE/'SOURCE.json').read_text(encoding='utf-8-sig'))
    require(source['architecture']=='ARM64' and source['source_url']=='https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/archive-virtio/virtio-win-0.1.302-1/virtio-win-0.1.302.iso','Unexpected driver source')
    triton=ROOT/'build/viogpu3d-arm64x-v0.5/drivers/viogpu3d'
    triton_files=sorted(p for p in triton.iterdir() if p.is_file())
    require(len(triton_files)==11,'Incomplete ARM64X driver package')
    inputs=dict(hashes)
    for p in triton_files:inputs['3d/'+p.name]=sha(p)
    for name in ('Install3D.ps1','Install3D.cmd','Rollback3D.ps1','Rollback3D.cmd','README.md'):
        inputs['3d/'+name]=sha(ROOT/'drivers/windows/neptune'/name)
    inputs['README.md']=sha(ROOT/'drivers/windows/virtio-display/README.md')
    inputs['LICENSE.txt']=sha(ROOT/'drivers/windows/virtio-display/LICENSE')
    inputs['builder']=sha(ROOT/'tools/driver_media_main.c')
    inputs['CollectCrash.ps1']=sha(ROOT/'drivers/windows/logs/CollectCrash.ps1')
    inputs['CollectLogs.cmd']=sha(ROOT/'drivers/windows/logs/CollectLogs.cmd')
    inputs['PrepareDisplay.ps1']=sha(ROOT/'drivers/windows/logs/PrepareDisplay.ps1')
    inputs['PrepareDisplay.cmd']=sha(ROOT/'drivers/windows/logs/PrepareDisplay.cmd')
    for name in ('DisplayTopology.cs','SetVirtioDisplay.ps1','SetVirtioDisplay.cmd'):
        inputs[name]=sha(ROOT/'drivers/windows/logs'/name)
    inputs['日志说明.txt']=sha(ROOT/'drivers/windows/logs/README.md')
    fingerprint=hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()
    if (BUNDLE/'manifest.json').is_file() and (BUNDLE/'drivers.img').is_file():
        previous=json.loads((BUNDLE/'manifest.json').read_text())
        if previous.get('input_sha256')==fingerprint and (BUNDLE/'drivers.img').stat().st_size==64<<20 and sha(BUNDLE/'drivers.img')==previous['image_sha256']:
            print('Verified existing official ARM64 display media; preserving its disk identity.')
            return
    BUNDLE.mkdir(parents=True,exist_ok=True)
    subprocess.run(['make','build/driver_media'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='display-media-') as tmp:
        temp=Path(tmp)
        shutil.copy2(ROOT/'drivers/windows/virtio-display/LICENSE',temp/'LICENSE.txt')
        shutil.copy2(ROOT/'drivers/windows/virtio-display/README.md',temp/'README.md')
        files=[PACKAGE/n for n in sorted(hashes)]+[PACKAGE/'SHA256SUMS.txt',temp/'LICENSE.txt',temp/'README.md']
        script=(ROOT/'drivers/windows/logs/CollectCrash.ps1').read_text(encoding='utf-8-sig')
        (temp/'CollectCrash.ps1').write_bytes(b'\xef\xbb\xbf'+script.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'))
        command=(ROOT/'drivers/windows/logs/CollectLogs.cmd').read_text()
        (temp/'一键收集日志.cmd').write_bytes(command.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'))
        shutil.copy2(ROOT/'drivers/windows/logs/README.md',temp/'日志说明.txt')
        files += [temp/'CollectCrash.ps1',temp/'一键收集日志.cmd',temp/'日志说明.txt']
        script=(ROOT/'drivers/windows/logs/PrepareDisplay.ps1').read_text(encoding='utf-8-sig')
        (temp/'PrepareDisplay.ps1').write_bytes(b'\xef\xbb\xbf'+script.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'))
        command=(ROOT/'drivers/windows/logs/PrepareDisplay.cmd').read_text()
        (temp/'准备显示驱动与蓝屏记录.cmd').write_bytes(command.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'))
        files += [temp/'PrepareDisplay.ps1',temp/'准备显示驱动与蓝屏记录.cmd']
        script=(ROOT/'drivers/windows/logs/SetVirtioDisplay.ps1').read_text(encoding='utf-8-sig')
        (temp/'SetVirtioDisplay.ps1').write_bytes(b'\xef\xbb\xbf'+script.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'))
        command=(ROOT/'drivers/windows/logs/SetVirtioDisplay.cmd').read_text()
        (temp/'将VirtIO设为唯一主屏.cmd').write_bytes(command.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'))
        shutil.copy2(ROOT/'drivers/windows/logs/DisplayTopology.cs',temp/'DisplayTopology.cs')
        files += [temp/'SetVirtioDisplay.ps1',temp/'将VirtIO设为唯一主屏.cmd',temp/'DisplayTopology.cs']
        for name in ('Install3D.ps1','Rollback3D.ps1'):
            script=(ROOT/'drivers/windows/neptune'/name).read_text(encoding='utf-8-sig')
            (temp/name).write_bytes(b'\xef\xbb\xbf'+script.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'))
            files.append(temp/name)
        for source,name in (('Install3D.cmd','3d.cmd'),('Rollback3D.cmd','Rollback3D.cmd')):
            text=(ROOT/'drivers/windows/neptune'/source).read_text()
            (temp/name).write_bytes(text.replace('\r\n','\n').replace('\n','\r\n').encode('utf-8'));files.append(temp/name)
        shutil.copy2(ROOT/'drivers/windows/neptune/README.md',temp/'3D-README.md')
        files += triton_files+[temp/'3D-README.md']
        image=temp/'drivers.img' 
        subprocess.run([str(ROOT/'build/driver_media'),str(image),'--label','WINIOSDRV',*[str(p) for p in files]],check=True)
        manifest=dict(image_sha256=sha(image),image_bytes=image.stat().st_size,
                      input_sha256=fingerprint,
                      source_version='virtio-win-0.1.302-1',verified_run=43713195856,
                      kernel_signature_verified=True,direct3d_implemented=False,neptune_driver_included=True,neptune_driver_version="100.6.101.58000",guest_3d_device_validated=False)
        os.replace(image,BUNDLE/'drivers.img')
        (BUNDLE/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        shutil.copy2(temp/'README.md',BUNDLE/'README.md')
        shutil.copy2(temp/'LICENSE.txt',BUNDLE/'LICENSE.txt')
    print('Staged official ARM64 VirtIO display driver, original catalog preserved.')

if __name__=='__main__':main()
