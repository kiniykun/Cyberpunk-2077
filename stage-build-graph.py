"""Stage original build definitions so Premake can resolve the real usage graph.

Premake may generate dependency sources; every writable path stays in this copy.
No game sources or asset files are copied or modified by this script.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parent
SOURCE = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev')
OUT = ROOT / 'build-tools' / 'premake' / 'Mainline' / 'dev'
records = []
for source in SOURCE.rglob('*.lua'):
    relative = source.relative_to(SOURCE)
    destination = OUT / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    raw = source.read_bytes()
    destination.write_bytes(raw)
    records.append({'relative': relative.as_posix(), 'sha256': hashlib.sha256(raw).hexdigest()})
(ROOT / 'build-tools/premake/source-manifest.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
wrapper = OUT / 'src' / 'browser-graph.lua'
wrapper.write_text('''
-- Metadata generation can validate source paths against the untouched depot.
-- Build definitions and generated outputs remain in the staged tree.
local nativeIsFile = os.isfile
local stagedDev = path.getabsolute("../..") .. "/"
local originalDev = "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/"
os.isfile = function(filename)
    if nativeIsFile(filename) then return true end
    local absolute = path.getabsolute(filename)
    if absolute:sub(1, #stagedDev):lower() == stagedDev:lower() then
        return nativeIsFile(originalDev .. absolute:sub(#stagedDev + 1))
    end
    return false
end
newaction {
    trigger = "cpwebgraph",
    description = "Export original cpLauncher resolved dependency graph",
    onProject = function(prj)
        if prj.name ~= "cpLauncher" then return end
        local output = assert(io.open("browser-module-graph.txt", "w"))
        local entries = {}
        for name, info in pairs(prj.resolvedUses or {}) do
            table.insert(entries, name .. "\\t" .. tostring(info.type))
        end
        table.sort(entries)
        for _, entry in ipairs(entries) do output:write(entry .. "\\n") end
        output:close()
    end,
}
include "premake5.lua"
''', encoding='utf-8')
print(f'Staged {len(records)} original Lua build definitions under {OUT}')
