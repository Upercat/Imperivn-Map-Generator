"""Actual hidden-window refusal cases: no installation and damaged/missing inputs."""
from pathlib import Path
import argparse
import json
import struct
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--exe',type=Path,required=True)
parser.add_argument('--game',type=Path,required=True)
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
if not args.game.is_dir(): raise SystemExit(77)
args.out.mkdir(parents=True,exist_ok=True)
bad=args.out/'invalid.dmo'
bad.write_bytes(b'not a replay')
huge=args.out/'hostile.dmo'
huge.write_bytes(b'LZIS'+struct.pack('<II',0xffffffff,1)+b'\x02\x00'+struct.pack('<I',18))
startup=subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow=0
for name,game,input_file in [
    ('missing-game',args.out/'no-game',None),
    ('invalid-file',args.game,bad),
    ('missing-file',args.game,args.out/'no-file.dmo'),
    ('hostile-file',args.game,huge),
]:
    report_file=args.out/(name+'.json')
    screenshot=args.out/(name+'.bmp')
    export=args.out/(name+'.BFHP')
    command=[str(args.exe),'--game',str(game),'--lang','en','--screenshot',str(screenshot),
        '--ui-report',str(report_file),'--export',str(export)]
    if input_file: command.append(str(input_file))
    subprocess.run(command,startupinfo=startup,check=True,timeout=25)
    report=json.loads(report_file.read_text())
    assert report['has_error'] and not report['can_save'] and not export.exists(), (name,report)
    assert report['can_generate'] == (name != 'missing-game'), (name,report)
    assert screenshot.is_file() and report['fits_work_area'], (name,report)
    try:
        from PIL import Image
        Image.open(screenshot).save(screenshot.with_suffix('.png'))
    except ImportError: pass
    print('PASS',name,json.dumps(report),flush=True)
