"""Generate the original TweakDB scanner without changing the source depot."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parent
depot = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev')
grammar = depot/'src/common/gameTweakDB/src/tdb.flex'
generator = depot/'external/flex/bin/win_flex.exe'
output = root/'generated/gameTweakDB/tdb_flex.cxx'
output.parent.mkdir(parents=True, exist_ok=True)
temporary = output.with_suffix('.building.cxx')
subprocess.run([str(generator), '-o', str(temporary), str(grammar)], check=True)
generated = temporary.read_bytes()
if not generated or b'tdblex' not in generated:
    raise RuntimeError('Original scanner generation produced invalid output')
temporary.replace(output)
manifest = {
    'Grammar': str(grammar),
    'GrammarSHA256': hashlib.sha256(grammar.read_bytes()).hexdigest(),
    'Generator': str(generator),
    'GeneratorSHA256': hashlib.sha256(generator.read_bytes()).hexdigest(),
    'Output': str(output),
    'OutputSHA256': hashlib.sha256(generated).hexdigest(),
    'GeneratedFromOriginalGrammar': True,
    'RuntimeVerified': False,
}
(root/'build/tweakdb-lexer-generation.json').write_text(json.dumps(manifest, indent=2))
print('Generated original TweakDB scanner:', output)
