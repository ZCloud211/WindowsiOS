#!/usr/bin/env python3
"""Read collected Windows diagnostics from a local COPY of the tools disk.

Never mounts or writes the image. No files or events are uploaded anywhere.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parent.parent

def dump_header(data):
    if len(data)>=0x60 and data[:8]==b'PAGEDU64':
        return dict(format='Windows kernel 64-bit',machine=hex(struct.unpack_from('<I',data,0x30)[0]),
                    bugcheck=hex(struct.unpack_from('<I',data,0x38)[0]),
                    parameters=[hex(x) for x in struct.unpack_from('<4Q',data,0x40)])
    if len(data)>=0x3c and data[:8]==b'PAGEDUMP':
        return dict(format='Windows kernel 32-bit',machine=hex(struct.unpack_from('<I',data,0x20)[0]),
                    bugcheck=hex(struct.unpack_from('<I',data,0x28)[0]),
                    parameters=[hex(x) for x in struct.unpack_from('<4I',data,0x2c)])
    return dict(format='unrecognized',signature=data[:8].hex())

def events(text):
    try:root=ET.fromstring(text)
    except ET.ParseError:
        try:root=ET.fromstring('<Events>'+text+'</Events>')
        except ET.ParseError:return []
    found=[]
    nodes=[root] if root.tag.endswith('Event') else root.findall('.//{*}Event')
    for event in nodes:
        event_id=event.find('./{*}System/{*}EventID')
        created=event.find('./{*}System/{*}TimeCreated')
        values={d.get('Name',str(n)):d.text for n,d in enumerate(event.findall('.//{*}EventData/{*}Data'))}
        found.append(dict(event_id=event_id.text if event_id is not None else None,
                          time=created.get('SystemTime') if created is not None else None,data=values))
    return found

def safe(name):
    return bool(name) and name not in ('.','..') and not any(c in name for c in '/\\\x00:') and len(name)<256

def read(image,destination,preparation=False,display_setup=False):
    # Reuse the existing independent read-only FAT verifier, not FatFs writer.
    spec=importlib.util.spec_from_file_location('fat_verifier',ROOT/'tests/test_iso_import.py')
    verifier=importlib.util.module_from_spec(spec);spec.loader.exec_module(verifier)
    fs=verifier.Fat32(image)
    summary={'image':str(image),'files':[],'dump_headers':{},'events':[]}
    try:
        if preparation:
            name='PREPLOGS';source=name
            _,folder,_,directory=fs.lookup(source)
            if not directory:raise ValueError('PREPLOGS is not a directory')
        else:
            root='DISPLAYLOGS' if display_setup else 'CRASHLOGS'
            _,cluster,_,directory=fs.lookup(root)
            if not directory:raise ValueError(root+' is not a directory')
            runs=sorted((entry for entry in fs.entries(cluster) if entry[3] and safe(entry[0])),reverse=True)
            runs=[r for r in runs if r[0] not in ('.','..')]
            if not runs:raise ValueError('No completed collection folder found')
            name=next((r[0] for r in runs if any(e[0].upper()=='DONE.TXT' for e in fs.entries(r[1]))),None)
            if not name:raise ValueError('Collection has not finished; '+root+' has no DONE.TXT')
            source=root+'/'+name
            _,folder,_,_=fs.lookup(source)
        output=destination/name;output.mkdir(parents=True,exist_ok=True)
        total=0
        for filename,first,size,is_dir in fs.entries(folder):
            if is_dir or not safe(filename):continue
            if size>8<<20 or total+size>32<<20:raise ValueError('Collected data exceeds expected size')
            data=fs.contents(source+'/'+filename);total+=len(data)
            path=output/filename
            if path.is_symlink():raise ValueError('Refusing to replace a symlink')
            path.write_bytes(data);summary['files'].append(dict(name=filename,size=len(data)))
            if filename.upper().endswith(('.DMP','.HEAD')):summary['dump_headers'][filename]=dump_header(data)
            if filename.upper()=='EVENTS.XML':summary['events']=events(data.decode('utf-8-sig',errors='replace'))
            if filename.upper()=='RESULT.JSON':summary['display_setup' if display_setup else 'collector']=json.loads(data.decode('utf-8-sig'))
            if filename.upper()=='PREP.JSON':summary['preparation']=json.loads(data.decode('utf-8-sig'))
        summary['directory']=str(output)
        (output/'ANALYSIS.json').write_text(json.dumps(summary,indent=2,ensure_ascii=False)+'\n')
        return summary
    finally:fs.file.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('--output',type=Path,default=ROOT/'work/windows-crash-logs')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--preparation',action='store_true',help='Read PREPLOGS driver staging and crash settings results')
    mode.add_argument('--display-setup',action='store_true',help='Read latest DISPLAYLOGS topology setup and rollback results')
    args=parser.parse_args()
    print(json.dumps(read(args.image,args.output,args.preparation,args.display_setup),ensure_ascii=False,indent=2))
