"""Headless pipeline: convert newly observed runtime shader misses and add them to the material package.

Usage (emsdk python):
  add-observed-shaders.py "0:10052:hair" "0:12298:hair" [--no-package]
Each argument is stage:bytes:name exactly as printed by
"CP2077 WebGPU: graphics shader has no exact original-bytecode conversion: stage=S bytes=B name=N".

Steps: convert-native-cache-shaders.py --material-observed (on a synthetic miss log), merge the new rows into
build/shaders/native-observed-material-conversion.json without dropping earlier rows, prepare only the new hashes,
validate them on a real GPU with Deno (tools/headless/validate-wgsl.ts), then merge + regenerate the package.
"""
import json, subprocess, sys, shutil
from pathlib import Path

ROOT = Path(r'D:\2077')
PY = sys.executable
WORK = Path(r'C:\Users\Brady\Documents\Codex\2026-10-10\hi-you-are-participating-in-a\work')
CONV = ROOT / 'build/shaders/native-observed-material-conversion.json'
PREP = ROOT / 'build/shaders/native-observed-material-graphics-binding-preparation.json'


def run(args, **kw):
    print('>', ' '.join(str(a) for a in args), flush=True)
    return subprocess.run([str(a) for a in args], cwd=kw.pop('cwd', ROOT), **kw)


def main():
    specs = [a for a in sys.argv[1:] if not a.startswith('--')]
    package = '--no-package' not in sys.argv
    messages = []
    for spec in specs:
        stage, size, name = spec.split(':', 2)
        messages.append({'message': f'CP2077 WebGPU: graphics shader has no exact original-bytecode conversion: stage={stage} bytes={size} name={name}'})
    synthetic = ROOT / 'build/headless/observed-misses.json'
    synthetic.parent.mkdir(parents=True, exist_ok=True)
    synthetic.write_text(json.dumps({'errors': messages}))

    previous = json.loads(CONV.read_text())
    shutil.copyfile(CONV, CONV.with_suffix('.before-add.json'))
    captured = next((a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--captured=')), None)
    extra = ['--captured', captured] if captured else []
    if captured and not specs:
        messages.append({'message': 'CP2077 WebGPU: graphics shader has no exact original-bytecode conversion: stage=1 bytes=0 name=captured'})
        synthetic.write_text(json.dumps({'errors': messages}))
    if run([PY, ROOT / 'convert-native-cache-shaders.py', '--material-observed', synthetic, *extra]).returncode:
        shutil.copyfile(CONV.with_suffix('.before-add.json'), CONV); sys.exit('conversion failed')
    added = json.loads(CONV.read_text())
    rows = {r['Hash']: r for r in previous['Shaders']}
    new_hashes = []
    for r in added['Shaders']:
        if r['Hash'] not in rows: new_hashes.append(r['Hash'])
        rows[r['Hash']] = r
    previous['Shaders'] = sorted(rows.values(), key=lambda r: int(r['Hash']))
    previous['OriginalPrograms'] = len(rows)
    previous['SPIRVConverted'] = sum(bool(r.get('SPIRVConverted')) for r in rows.values())
    previous['WGSLValidated'] = sum(bool(r.get('WGSLValidated')) for r in rows.values())
    CONV.write_text(json.dumps(previous, indent=2))
    print('new conversion rows:', new_hashes)
    if not new_hashes: return

    run([PY, ROOT / 'prepare-native-static-graphics.py', '--material-observed', '--only', *new_hashes])
    prep = {r['Hash']: r for r in json.loads(PREP.read_text())['Shaders']}
    ok = [h for h in new_hashes if prep.get(h, {}).get('RegisterMappingVerified')]
    for h in new_hashes:
        if h not in ok: print('NOT PREPARED', h, prep.get(h, {}).get('PreparationError', '')[:300])
    if ok:
        deno = shutil.which('deno') or 'deno'
        run([deno, 'run', '-A', ROOT / 'tools/headless/validate-wgsl.ts', PREP, *ok])
    if package and ok:
        run([PY, WORK / 'merge-observed-material-shaders.py'])
        run([PY, ROOT / 'generate-static-graphics-package.py', '--material-ui'])
        run([PY, ROOT / 'float_texture_compat.py'])
        print('Package regenerated; recompile gpu-shader-web gpu-uniform-web gpu-api-command-web and relink.')


if __name__ == '__main__':
    main()
