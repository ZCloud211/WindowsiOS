#!/usr/bin/env python3
"""Keep RAMFB as UEFI GOP while leaving the PCI VirtIO GPU for Windows.

This narrowly transforms QEMU v9.2.0's pinned firmware: the VirtioGpuDxe FFS
file becomes a skipped padding file, retaining its contents and layout. Every
other byte in the decompressed DXE volume stays unchanged. Repack the standard
LZMA GUID section and its FFS header; preserve the original firmware and NVRAM.
"""
import hashlib
import json
import lzma
from pathlib import Path
import struct
import uuid

ROOT=Path(__file__).resolve().parent.parent
SOURCE_SHA256='47765fe344818cbc464b1c14ae658fb4b854f5c2ceffa982411731eb4865594d'
GPU_GUID=uuid.UUID('D6099B94-CD97-4CC5-8714-7F6312701A8A').bytes_le
LZMA_GUID=uuid.UUID('EE4E5898-3914-4259-9D6E-DC7BD79403CF').bytes_le

def require(value,message):
    if not value:raise ValueError(message)

def digest(data):return hashlib.sha256(data).hexdigest()

def volume(data,base):
    require(data[base+40:base+44]==b'_FVH','Invalid firmware volume')
    length=struct.unpack_from('<Q',data,base+32)[0]
    header=struct.unpack_from('<H',data,base+48)[0]
    require(header>=56 and header%2==0 and base+length<=len(data),'Invalid FV size')
    require(sum(struct.unpack_from('<'+'H'*(header//2),data,base))&65535==0,'Invalid FV header checksum')
    position=(base+header+7)&~7
    files=[]
    while position+24<=base+length and data[position:position+24]!=b'\xff'*24:
        size=int.from_bytes(data[position+20:position+23],'little')
        require(size!=0xffffff and size>=24 and position+size<=base+length,'Unsupported FFS size')
        h=bytearray(data[position:position+24]);h[17]=h[23]=0
        require(sum(h)&255==0,'Invalid FFS header checksum')
        if data[position+19]&0x40:
            require((sum(data[position+24:position+size])+data[position+17])&255==0,'Invalid FFS data checksum')
        else:require(data[position+17]==0xaa,'Invalid fixed FFS checksum')
        files.append((position,size,data[position+18],data[position:position+16]))
        position=(position+size+7)&~7
    return length,files

def header_checksum(header,payload):
    header[16]=0
    header[17]=(-sum(payload))&255 if header[19]&0x40 else 0xaa
    temporary=bytearray(header);temporary[17]=temporary[23]=0
    header[16]=(-sum(temporary))&255

def patch(source):
    require(len(source)==64<<20 and digest(source)==SOURCE_SHA256,'Refusing unknown firmware contents')
    base=source.index(b'_FVH')-40
    fv_length,files=volume(source,base)
    candidates=[x for x in files if x[2]==0x0b]
    require(len(candidates)==1,'Expected one compressed DXE volume')
    offset,size,_,_=candidates[0]
    require(all(x==255 for x in source[offset+size:base+fv_length]),'Unexpected file after DXE container')
    section=offset+24
    section_size=int.from_bytes(source[section:section+3],'little')
    require(source[section+3]==2 and source[section+4:section+20]==LZMA_GUID,'Unsupported DXE compression section')
    data_offset=struct.unpack_from('<H',source,section+20)[0]
    require(data_offset==24 and section_size+24==size,'Unexpected section layout')
    original=lzma.decompress(source[section+data_offset:section+section_size],format=lzma.FORMAT_ALONE)
    inner=original.index(b'_FVH')-40
    _,inner_files=volume(original,inner)
    matches=[x for x in inner_files if x[3]==GPU_GUID]
    require(len(matches)==1 and matches[0][2]==7,'Expected exactly one VirtioGpuDxe driver')
    target,target_size,_,_=matches[0]
    require('VirtioGpuDxe'.encode('utf-16le') in original[target:target+target_size],'Driver GUID/name mismatch')
    modified=bytearray(original)
    modified[target+18]=0xf0 # EFI_FV_FILETYPE_FFS_PAD: not a dispatchable driver.
    h=bytearray(modified[target:target+24])
    header_checksum(h,modified[target+24:target+target_size])
    modified[target:target+24]=h
    changes=[n for n in range(len(original)) if original[n]!=modified[n]]
    require(changes==[target+16,target+18],'Unexpected DXE volume change')
    _,after=volume(modified,inner)
    require(len(after)==len(inner_files),'DXE volume layout changed')
    require([(p,s,g) for p,s,t,g in after]==[(p,s,g) for p,s,t,g in inner_files],'Other module location changed')
    encoded=lzma.compress(modified,format=lzma.FORMAT_ALONE,filters=[dict(
        id=lzma.FILTER_LZMA1,dict_size=1<<24,lc=3,lp=0,pb=2,
        mode=lzma.MODE_NORMAL,nice_len=273,mf=lzma.MF_BT4)])
    encoded=encoded[:5]+len(modified).to_bytes(8,'little')+encoded[13:]
    require(lzma.decompress(encoded,format=lzma.FORMAT_ALONE)==modified,'LZMA round trip mismatch')
    sh=bytearray(source[section:section+data_offset])
    sh[:3]=(len(sh)+len(encoded)).to_bytes(3,'little')
    payload=sh+encoded
    outer=bytearray(source[offset:offset+24])
    outer[20:23]=(24+len(payload)).to_bytes(3,'little')
    header_checksum(outer,payload)
    replacement=outer+payload
    require(offset+len(replacement)<=base+fv_length,'Repacked firmware does not fit')
    result=bytearray(source)
    result[offset:base+fv_length]=replacement+b'\xff'*(base+fv_length-offset-len(replacement))
    _,checked=volume(result,base)
    require(len(checked)==len(files),'Outer firmware layout changed')
    require(result[:offset]==source[:offset] and result[base+fv_length:]==source[base+fv_length:],'Changed unrelated firmware')
    report=dict(source_sha256=SOURCE_SHA256,sha256=digest(result),size=len(result),
        skipped_driver_guid=str(uuid.UUID(bytes_le=GPU_GUID)),skipped_driver='VirtioGpuDxe',
        decompressed_changes=changes,retained_dxe_modules=len(after)-1,
        ramfb_boot=True,virtio_pci_available_to_windows=True)
    return bytes(result),report

def main():
    source=(ROOT/'work/uefi/Machines/Win11/code.fd').read_bytes()
    output,report=patch(source)
    directory=ROOT/'ios/Sources/Resources/Firmware'
    directory.mkdir(parents=True,exist_ok=True)
    (directory/'code-ramfb.fd').write_bytes(output)
    (directory/'ramfb-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Prepared RAMFB boot firmware; skipped VirtioGpuDxe, other decompressed DXE bytes unchanged.')
    print('Firmware SHA256:',report['sha256'])

if __name__=='__main__':main()
