"""Hidden application smoke test: native tabs/buttons, thumbnail selection, cancellation and exported bytes."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True, type=Path)
parser.add_argument('--core-test', required=True, type=Path)
parser.add_argument('--game', required=True, type=Path)
parser.add_argument('--fixture', required=True, type=Path)
parser.add_argument('--out', required=True, type=Path)
args = parser.parse_args()
if not args.game.is_dir() or not args.fixture.is_file():
    print('SKIP: owner-local game or recovery fixture missing')
    raise SystemExit(77)
args.out.mkdir(parents=True, exist_ok=True)
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0

cases = [
    ('gallery-es', ['--gallery', '--lang', 'es', '--select', '8', str(args.fixture)], 9, 8, 0x12345678, True, False),
    ('gallery-en-next', ['--gallery', '--lang', 'en', '--select', '8', '--ui-dpi', '120', '--next-batch'], 9, 8, 0x12345681, False, False),
    ('gallery-maximized', ['--gallery', '--lang', 'es', '--select', '8', '--ui-maximize'], 9, 8, 0x12345678, False, False),
    ('gallery-detail', ['--gallery', '--lang', 'es', '--select', '8', '--ui-maximize', '--ui-enlarge'], 9, 8, 0x12345678, False, False),
    ('gallery-detail-en', ['--gallery', '--lang', 'en', '--select', '0', '--ui-dpi', '120', '--ui-enlarge'], 9, 0, 0x12345678, False, False),
    ('gallery-cancel', ['--gallery', '--lang', 'es', '--select', '0', '--cancel-after', '3'], None, 0, 0x12345678, False, True),
    ('recover-en', ['--lang', 'en', str(args.fixture)], 0, -1, None, True, False),
]
for name, options, ready, selected, base, recovery, cancelled in cases:
    report_file = args.out / (name + '.json')
    bitmap = args.out / (name + '.bmp')
    volume = args.out / (name + '.BFHP')
    command = [str(args.exe), '--game', str(args.game), '--size', 'small', '--players', '2', '--seed', '12345678',
        '--screenshot', str(bitmap), '--ui-report', str(report_file), '--export', str(volume), *options]
    subprocess.run(command, startupinfo=startup, check=True, timeout=85)
    report = json.loads(report_file.read_text())
    assert report['selected'] == selected and report['recovery_retained'] == recovery, (name, report)
    assert report['detail_round_trip'], (name, 'detail navigation lost the selection or cached maps')
    assert report['enlarged'] == ('--ui-enlarge' in options), (name, report)
    if report['enlarged']:
        assert report['detail_pixels'] > report['image_pixels'] * 3, (name, report)
    assert report['fits_work_area'], (name, 'window extends outside the monitor work area')
    assert report['cancelled'] == cancelled and report['native_tabs'] and report['keyboard_selection'] and report['selection_matches'], (name, report)
    assert (report['ready'] == ready) if ready is not None else (3 <= report['ready'] < 9), (name, report)
    assert volume.is_file() and hashlib.sha256(volume.read_bytes()).hexdigest() == report['export_sha256'], (name, 'wrong export')
    assert bitmap.is_file() and bitmap.stat().st_size > 100_000, (name, 'missing screenshot')
    if base is not None:
        assert report['base_seed'] == base and report['seed'] == (base + selected) & 0xffffffff, (name, report)
    if name == 'gallery-es':
        default_image_pixels = report['image_pixels']
    if name == 'gallery-maximized':
        assert report['maximized'] and report['image_pixels'] > default_image_pixels, (name, report)
    if name == 'gallery-es':
        independent = args.out / 'independent-card-9.BFHP'
        subprocess.run([str(args.core_test), str(args.game), '--export', '12345678', str(independent), report['text_language']], check=True, timeout=45)
        assert independent.read_bytes() == volume.read_bytes(), 'UI exported a different map from the selected seed'
    try:
        from PIL import Image
        Image.open(bitmap).save(bitmap.with_suffix('.png'))
    except ImportError:
        pass
    print('PASS', name, json.dumps(report), flush=True)

print('PASS gallery UI: tab retention, selection, keyboard, next batch, cancellation, DPI, recovery and independent export parity')
