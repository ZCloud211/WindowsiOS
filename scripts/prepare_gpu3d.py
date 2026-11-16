#!/usr/bin/env python3
"""Stage the pinned native D3D11 renderer and the unmodified ARM64X driver."""
from pathlib import Path
import hashlib,json,plistlib,shutil,subprocess,zipfile
ROOT=Path(__file__).resolve().parent.parent
DEPS=ROOT/'ios/deps'
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
    native=ROOT/'work/gpu3d-deps/ios-prefix'
    renderer=ROOT/'work/gpu3d-deps/neptune-ios-prefix'
    for prefix,name in ((native,'libdxmt-native.dylib'),(renderer,'libvirglrenderer.a')):
        meta=json.loads((prefix/'BUILD.json').read_text())
        assert meta['platform']=='arm64-apple-ios17.0' and meta['sha256']==sha(prefix/'lib'/name)
    assert json.loads((native/'BUILD.json').read_text())['dxmt_commit']=='1fe2e752c98207518a99c9322482f63ebb9f0232'
    assert json.loads((renderer/'BUILD.json').read_text())['renderer_commit']=='dd0652b13e589c4c124ea79123a3adcd85173f4f'
    framework=DEPS/'GPU3D/DXMT.framework';framework.mkdir(parents=True,exist_ok=True)
    shutil.copy2(native/'lib/libdxmt-native.dylib',framework/'DXMT')
    subprocess.run(['xcrun','install_name_tool','-id','@rpath/DXMT.framework/DXMT',str(framework/'DXMT')],check=True)
    (framework/'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleExecutable='DXMT',CFBundleIdentifier='org.dxmt.native',
        CFBundleName='DXMT',CFBundlePackageType='FMWK',CFBundleShortVersionString='1.0',CFBundleVersion='1',
        MinimumOSVersion='17.0',CFBundleSupportedPlatforms=['iPhoneOS'])))
    (DEPS/'include/virgl').mkdir(parents=True,exist_ok=True);(DEPS/'lib').mkdir(parents=True,exist_ok=True)
    for p in (renderer/'include/virgl').glob('*.h'):shutil.copy2(p,DEPS/'include/virgl'/p.name)
    for name in ('libvirglrenderer.a','libepoxy.a'):shutil.copy2(renderer/'lib'/name,DEPS/'lib'/name)
    archive=ROOT/'work/viogpu3d-arm64x-v0.5.zip'
    assert sha(archive)=='155695ab7820f71e75c369aeae7b8c060691ba43646ac37edef72aa2428506d5'
    package=ROOT/'build/viogpu3d-arm64x-v0.5/drivers/viogpu3d'
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            if entry.is_dir():continue
            assert entry.filename.startswith('drivers/viogpu3d/') and '..' not in Path(entry.filename).parts
            data=z.read(entry)
            assert (package/Path(entry.filename).name).read_bytes()==data
    licenses=ROOT/'ios/Sources/Resources/Licenses';source=ROOT/'work/gpu-3d-sources'
    for a,b in ((source/'dxmt/LICENSE','DXMT-NOTICE.txt'),(source/'dxmt/COPYING.LIB','DXMT-LGPL-2.1.txt'),
                (source/'virglrenderer/COPYING','Neptune-renderer-COPYING.txt'),(source/'libepoxy/COPYING','libepoxy-COPYING.txt'),
                (source/'llvm-project/llvm/LICENSE.TXT','LLVM-LICENSE.txt')):
        shutil.copy2(a,licenses/b)
    record={'native_dxmt':json.loads((native/'BUILD.json').read_text()),'neptune':json.loads((renderer/'BUILD.json').read_text()),
        'windows_driver':'https://github.com/osy/kvm-guest-drivers-windows/releases/tag/v0.5',
        'windows_archive_sha256':sha(archive),'windows_signature':'Microsoft Windows Hardware Compatibility Publisher catalog; Turing Software EV binary; Windows kernel-policy verification passed in GitHub Actions run 43785214442',
        'windows_device_validated':False,'framework_sha256':sha(framework/'DXMT')}
    for part in ('native_dxmt','neptune'):record[part].pop('library',None)
    (DEPS/'GPU3D/BUILD.json').write_text(json.dumps(record,indent=2)+'\n')
    print('Staged iOS native DXMT framework, Neptune renderer and complete ARM64X driver package; Windows validation pending.')
if __name__=='__main__':main()
