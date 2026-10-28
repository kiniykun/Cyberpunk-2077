"""Preserve the original generator's complete launcher initialization source."""
from pathlib import Path
import hashlib
import json
import re

root = Path(__file__).resolve().parent
staged = root / 'build-tools/premake/Mainline/dev'
generated = staged / 'temp/cpwebgraph/games/cpLauncher/src/dependencies.cpp'
graph_file = staged / 'src/browser-module-graph.txt'
graph = dict(line.split('\t') for line in graph_file.read_text().splitlines())
text = generated.read_text(encoding='utf-8-sig')
registrations = re.findall(r'RED_INITIALIZE_MODULE\(\s*(\w+)\s*\)', text)
expected = {name for name, kind in graph.items() if kind == 'project' and (not name.startswith('red') or name == 'redReflection')}
if not expected or set(registrations) != expected or len(registrations) != len(expected):
    raise RuntimeError('Generated module initialization does not cover the resolved project graph')
old_include = '#include "../../../../../src/common/redSystem/include/redSystemPublic.h"'
if text.count(old_include) != 1:
    raise RuntimeError('Unexpected generated system include')
text = text.replace(old_include, '#include "common/redSystem/include/redSystemPublic.h"')
(root / 'src/module-dependencies.cpp').write_text(text, encoding='utf-8')
(root / 'build/module-graph.json').write_text(json.dumps({
    'generator': 'Original Premake executable and original usage.lua resolver',
    'configuration': 'Final / x64_Linux / gs=null; native project graph, browser platform adapters still required',
    'resolvedUses': graph,
    'initializationModules': registrations,
    'generatedSource': str(generated),
    'generatedSourceSHA256': hashlib.sha256(generated.read_bytes()).hexdigest(),
    'stagedSourceSHA256': hashlib.sha256(text.encode('utf-8')).hexdigest(),
    'adaptation': 'System public include changed to source-root-relative path only',
}, indent=2), encoding='utf-8')
print(f'Preserved original full launcher initialization: {len(registrations)} modules')
