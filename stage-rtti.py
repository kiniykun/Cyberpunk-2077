from pathlib import Path
import hashlib,json,re,os,sys,subprocess
ROOT=Path(__file__).resolve().parent
SOURCE=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\internal\RTTIGenerator\dev')
OUT=ROOT/'build-tools/rtti'
records=[]
for folder in ['include','src']:
    for source in (SOURCE/folder).iterdir():
        if source.suffix not in ['.h','.cpp']:continue
        raw=source.read_bytes()
        try:text=raw.decode('utf-8-sig')
        except UnicodeDecodeError:text=raw.decode('cp1252')
        text=text.replace('\r\n','\n').replace('#ifdef __linux__','#if defined(__linux__) || defined(__EMSCRIPTEN__)')
        text=text.replace('#include <experimental/filesystem>','#include <filesystem>').replace('namespace fs = std::experimental::filesystem;', 'namespace fs = std::filesystem;')
        text=text.replace('int main( int argc, const char* argv[] )', 'int main( int argc, char* argv[] )')
        if source.name=='main.cpp':text=text.replace('return errorCode;', 'return errorCode ? errorCode : (Log::ErrorCount ? 1 : 0);')
        if source.name in ['commandLineParser.cpp','commandLineParser.h']:
            text=text.replace('const char* argv[]','const char* const argv[]')
        text=text.replace('async( ParseFile, filePath )','std::async( std::launch::deferred, ParseFile, filePath )')
        if source.name=='build.h':text='#include <vector>\n#include <string>\n#include <algorithm>\n#include <cstdio>\n#include <chrono>\n'+text
        output=OUT/folder/source.name;output.parent.mkdir(parents=True,exist_ok=True);output.write_text(text,encoding='utf-8')
        records.append({'source':str(source),'originalSHA256':hashlib.sha256(raw).hexdigest(),'adaptedSHA256':hashlib.sha256(output.read_bytes()).hexdigest()})
(OUT/'source-manifest.json').write_text(json.dumps(records,indent=2))
# Feed the original generator a project containing the original portable
# compilation units listed in the original module build definition.
module=sys.argv[1] if len(sys.argv)>1 else 'engine'
graph = json.loads((ROOT / 'build/module-graph.json').read_text(encoding='utf-8-sig'))
if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]*', module) or graph['resolvedUses'].get(module) != 'project':
    raise ValueError('Unknown original launcher module')
engine=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common')/('renderer' if module=='render' else module)
if module == 'cp':
    engine=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\games\cp')
units=sorted(set(re.findall(r'"(src/[^"\r\n]+\.cpp)"',(engine/'premake5.lua').read_text(encoding='utf-8-sig'))))
if module == 'input':
    original=(engine/'premake5.lua').read_text(encoding='utf-8-sig')
    base_definition=re.search(r'input_files\s*=\s*\{(.*?)\n\}',original,re.S)
    if not base_definition: raise RuntimeError('Original portable input source table missing')
    units=sorted(set(re.findall(r'"(src/[^"\r\n]+\.cpp)"',base_definition.group(0))))
units=[unit for unit in units if not re.search(r'(Win(?:32|64|dows)?(?=[A-Z_./]|$)|Orbis|Durango|PS4|XboxOne)',unit)]
project=OUT/(module+'.vcxproj')
project.write_text('<Project>\n'+''.join('<ClCompile Include="'+os.path.relpath(engine/unit,OUT).replace('\\','/')+'" />\n' for unit in units)+'</Project>\n')
if module == 'input':
    text=project.read_text()
    text=text.replace('</Project>', '<ClCompile Include="'+os.path.relpath(ROOT/'src/input-web.cpp',OUT).replace('\\','/')+'" />\n</Project>')
    project.write_text(text)
if module == 'gameServices':
    subprocess.run([sys.executable,str(ROOT/'stage-game-services-web.py')],check=True)
    text=project.read_text()
    text=text.replace('</Project>', '<ClCompile Include="'+os.path.relpath(ROOT/'src/game-services-web.cpp',OUT).replace('\\','/')+'" />\n</Project>')
    project.write_text(text)
additional=[]
if module=='input':additional.append(str(ROOT/'src/input-web.cpp'))
if module=='gameServices':additional.append(str(ROOT/'src/game-services-web.cpp'))
(OUT/(module+'-project-input.json')).write_text(json.dumps({'originalPremake':str(engine/'premake5.lua'),'selectedUnits':units,'additionalPortUnits':additional,'selection':'portable original sources plus recorded browser adapters; native platform source files excluded'},indent=2))
print('Staged original RTTI generator and',len(units),module,'source units')







