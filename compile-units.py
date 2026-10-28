"""Compile independent original units with bounded compiler concurrency.
No engine behavior is replaced; all selected units must succeed for an archive.
"""
import concurrent.futures,json,os,subprocess,sys,time
from pathlib import Path
manifest_path=Path(sys.argv[1]);manifest=json.loads(manifest_path.read_text(encoding='utf-8-sig'))
status_path=Path(manifest['StatusPath'])
results={u['Source']:u for u in manifest['Retained']}
def publish():
    units=[results[u['Source']] for u in manifest['Units'] if u['Source'] in results]
    status={'Module':manifest['Module'],'Sources':len(manifest['Units']),
            'Compiled':sum(u['Compiled'] for u in units),'Complete':False,
            'BuildInProgress':True,'Units':units}
    temporary=status_path.with_suffix('.progress.tmp')
    temporary.write_text(json.dumps(status,indent=2),encoding='utf-8')
    temporary.replace(status_path)
def compile_one(unit):
    args=unit.get('Args',manifest['CommonArgs'])[:]
    if unit['Input'].endswith('.c'):
        args=[a for a in args if a!='-std=c++17']+['-x','c','-std=c11']
    with Path(unit['Log']).open('wb') as log:
        proc=subprocess.run([sys.executable,manifest['Compiler'],*args,'-c',unit['Input'],'-o',unit['Object']],stdout=log,stderr=subprocess.STDOUT)
    outcome={k:unit[k] for k in ['Source','Object','Log']}
    outcome['Compiled']=proc.returncode==0
    if proc.returncode:
        errors=[line for line in Path(unit['Log']).read_text(encoding='utf-8',errors='replace').splitlines() if 'error:' in line]
        print('\n'.join(errors[:3]),flush=True)
    return outcome
publish()
pending=[u for u in manifest['Units'] if u['Source'] not in results]
with concurrent.futures.ThreadPoolExecutor(max_workers=manifest['Workers']) as pool:
    futures=[pool.submit(compile_one,u) for u in pending]
    for future in concurrent.futures.as_completed(futures):
        unit=future.result();results[unit['Source']]=unit;publish()
ordered=[results[u['Source']] for u in manifest['Units']]
Path(manifest['ResultsPath']).write_text(json.dumps(ordered,indent=2),encoding='utf-8')
