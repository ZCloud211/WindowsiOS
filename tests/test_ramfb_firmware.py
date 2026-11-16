#!/usr/bin/env python3
"""Check the boot regression fix without starting a VM or touching NVRAM."""
import importlib.util
import lzma
from pathlib import Path

ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('ramfb_firmware',ROOT/'scripts/prepare_ramfb_firmware.py')
firmware=importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)

def unpack(data):
    base=data.index(b'_FVH')-40
    _,files=firmware.volume(data,base)
    offset,size,kind,guid=next(x for x in files if x[2]==11)
    section=offset+24
    length=int.from_bytes(data[section:section+3],'little')
    return lzma.decompress(data[section+24:section+length],format=lzma.FORMAT_ALONE)

def main():
    path=ROOT/'work/uefi/Machines/Win11/code.fd'
    original=path.read_bytes()
    prepared,report=firmware.patch(original)
    before,after=unpack(original),unpack(prepared)
    inner=after.index(b'_FVH')-40
    _,files=firmware.volume(after,inner)
    drivers=[(p,n,g) for p,n,t,g in files if t==7]
    assert not any(g==firmware.GPU_GUID for _,_,g in drivers)
    assert any('QemuRamfbDxe'.encode('utf-16le') in after[p:p+n] for p,n,g in drivers)
    assert len(before)==len(after)
    differences=[n for n in range(len(before)) if before[n]!=after[n]]
    assert differences==report['decompressed_changes'] and len(differences)==2
    assert before[differences[1]]==7 and after[differences[1]]==0xf0
    assert path.read_bytes()==original
    unknown=bytearray(original);unknown[0]^=1
    try:firmware.patch(unknown)
    except ValueError:pass
    else:raise AssertionError('Unknown firmware was accepted')
    print('PASS: VirtioGpuDxe not dispatchable; original QemuRamfbDxe remains a driver')
    print('PASS: only driver type/header checksum differ in the decompressed DXE volume')
    print('PASS: compressed round trip, FV/FFS checksums, layout and source identity verified')
    print('No VM started; original firmware and NVRAM not modified.')

if __name__=='__main__':main()
