"""Build a clean evaluation ZIP from the validated executable and explicit public documents."""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import zipfile

def inspect_pe(path):
    data=path.read_bytes()
    pe=struct.unpack_from('<I',data,0x3c)[0]
    if data[pe:pe+4] != b'PE\0\0': raise ValueError('not PE')
    machine,sections=struct.unpack_from('<HH',data,pe+4)
    optional_size=struct.unpack_from('<H',data,pe+20)[0]
    opt=pe+24
    if struct.unpack_from('<H',data,opt)[0] != 0x20b: raise ValueError('not PE32+')
    table=opt+optional_size
    def offset(rva):
        for index in range(sections):
            entry=table+40*index
            virtual_size,virtual,raw_size,raw=struct.unpack_from('<IIII',data,entry+8)
            if virtual <= rva < virtual+max(virtual_size,raw_size): return raw+rva-virtual
        raise ValueError('RVA outside sections')
    rva,size=struct.unpack_from('<II',data,opt+112+8)
    imports=[]
    if rva:
        position=offset(rva)
        for index in range(size//20):
            entry=struct.unpack_from('<IIIII',data,position+20*index)
            if not any(entry): break
            name=offset(entry[3]); end=data.index(0,name)
            imports.append(data[name:end].decode('ascii'))
    dll_flags=struct.unpack_from('<H',data,opt+70)[0]
    certificate=struct.unpack_from('<II',data,opt+112+8*4)
    return dict(machine=hex(machine),architecture='x64' if machine==0x8664 else 'other',
        imports=sorted(imports),aslr=bool(dll_flags&0x40),nx=bool(dll_flags&0x100),
        high_entropy_aslr=bool(dll_flags&0x20),certificate_table_bytes=certificate[1])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',required=True,type=Path)
    parser.add_argument('--out',required=True,type=Path)
    args=parser.parse_args()
    project=Path(__file__).resolve().parents[1]
    subprocess.run([sys.executable,'-I',str(project/'tools/check_strings.py'),str(args.exe)],check=True)
    pe=inspect_pe(args.exe)
    assert pe['architecture']=='x64' and pe['aslr'] and pe['nx'],pe
    assert not any('vcruntime' in name.lower() or 'msvcp' in name.lower() for name in pe['imports']),pe
    contents={'imperivm-map-extractor.exe':args.exe.read_bytes(),
        'README.txt':(project/'docs/USER_GUIDE.txt').read_bytes(),
        'CHANGELOG.txt':(project/'CHANGELOG.txt').read_bytes()}
    for name,data in contents.items():
        if name.endswith('.txt'):
            for forbidden in [b'C:\\Users\\',b'bakcup_dev',b'randommap_captures']:
                assert forbidden not in data,(name,forbidden)
    sums=''.join(hashlib.sha256(data).hexdigest()+'  '+name+'\n' for name,data in sorted(contents.items()))
    contents['SHA256SUMS.txt']=sums.encode()
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.out,'w',zipfile.ZIP_DEFLATED) as archive:
        for name,data in contents.items(): archive.writestr(name,data)
    with zipfile.ZipFile(args.out) as archive:
        assert set(archive.namelist())==set(contents) and archive.testzip() is None
    report={'version':'0.2.3-beta','status':'evaluation candidate; manual acceptance and license choice pending',
        'sha256':hashlib.sha256(args.out.read_bytes()).hexdigest(),'members':sorted(contents),'pe':pe}
    args.out.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))

if __name__=='__main__': main()
