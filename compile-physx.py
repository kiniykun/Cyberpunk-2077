"""Build original PhysX components selected from their shipped Linux makefiles."""
import argparse,hashlib,json,re,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
VENDOR=Path(r"D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\PhysX342")
SDK=Path(r"D:\cp2077SourceCode\tools\emsdk")
MAKE=VENDOR/'PhysX_3.4/Source/compiler/linux64'
parser=argparse.ArgumentParser();parser.add_argument('--components',nargs='+');parser.add_argument('--retry-failed',action='store_true');args=parser.parse_args()
components=re.findall(r'^include Makefile\.(\w+)\.mk$',(MAKE/'Makefile').read_text(),re.M)
selected=args.components or components
if any(c not in components for c in selected):raise ValueError('Unknown original PhysX component')
subprocess.run([sys.executable,str(ROOT/'stage-port.py')],check=True)
failed=False
for component in selected:
    original=MAKE/f'Makefile.{component}.mk';text=original.read_text()
    sources=[(MAKE/relative.strip()).resolve() for relative in re.findall(r'^'+component+r'_cppfiles\s*\+=\s*(.+)$',text,re.M)]
    includes=[(MAKE/relative.strip()).resolve() for relative in re.findall(r'^'+component+r'_release_hpaths\s*\+=\s*(.+)$',text,re.M)]
    definitions=re.findall(r'^'+component+r'_release_defines\s*\+=\s*(.+)$',text,re.M)
    if not sources or any(not source.is_file() for source in sources):raise RuntimeError('Original source list incomplete: '+component)
    if any('$' in d for d in definitions):raise RuntimeError('Unexpanded original release definition')
    module='physx-'+component;out=ROOT/'build/modules'/module;out.mkdir(parents=True,exist_ok=True)
    flags=['-std=c++11','-O0','-g','-fshort-wchar','-pthread','-msimd128','-msse4.1','-sMEMORY64=1','-ferror-limit=5','-DPX_PHYSX_STATIC_LIB','-ivfsoverlay',str(ROOT/'clang-vfs.json')]
    flags+=['-I'+str(p) for p in includes]+['-D'+d.strip() for d in definitions]
    units=[{'Source':str(source),'Input':str(source),'Object':str(out/f'{index:04d}-{source.stem}.o'),'Log':str(out/f'{index:04d}-{source.stem}.log')} for index,source in enumerate(sources)]
    status=out/'result.json';retained=[]
    if args.retry_failed and status.exists():
        prior=json.loads(status.read_text(encoding='utf-8-sig'))
        if prior.get('CompilerArgs')!=flags:raise RuntimeError('Compiler arguments changed; use a full build')
        retained=[u for u in prior['Units'] if u['Compiled'] and Path(u['Object']).is_file()]
    manifest={'Module':module,'Compiler':str(SDK/'upstream/emscripten/em++.py'),'CommonArgs':flags,'Workers':4,'Units':units,'Retained':retained,'StatusPath':str(status),'ResultsPath':str(out/'compile-results.json')}
    manifest_path=out/'compile-jobs.json';manifest_path.write_text(json.dumps(manifest,indent=2))
    subprocess.run([sys.executable,str(ROOT/'compile-units.py'),str(manifest_path)],check=True)
    results=json.loads((out/'compile-results.json').read_text());compiled=sum(u['Compiled'] for u in results);complete=compiled==len(units)
    library=ROOT/'build/lib'/f'lib{module}.a';library.parent.mkdir(parents=True,exist_ok=True)
    if complete:
        temporary=library.with_suffix('.building.a');temporary.unlink(missing_ok=True)
        subprocess.run([sys.executable,str(SDK/'upstream/emscripten/emar.py'),'rcs',str(temporary),*[u['Object'] for u in results]],check=True)
        temporary.replace(library)
    final={'Module':module,'Sources':len(units),'Compiled':compiled,'Complete':complete,'Library':str(library) if complete else None,'Units':results,'CompilerArgs':flags,'WcharBytes':2,'OriginalMakefile':str(original),'OriginalMakefileSHA256':hashlib.sha256(original.read_bytes()).hexdigest(),'RuntimeVerified':False}
    status.write_text(json.dumps(final,indent=2));print(f'{module}: {compiled}/{len(units)} compiled; complete={complete}',flush=True)
    failed |= not complete
sys.exit(1 if failed else 0)
