"""Build the browser output layer against the source-built original Wwise core."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

root = Path(__file__).resolve().parent
core = json.loads((root / 'build/modules/wwise-sound-core/result.json').read_text())
if not core['Complete']:
    raise SystemExit('Compile the original audio core before its browser output layer')
manifest = json.loads((root / 'build/modules/wwise-sound-core/compile-jobs.json').read_text())
source = root / 'src/wwise-audio-web.cpp'
object_path = root / 'build/objects/wwise-audio-web.o'
log_path = root / 'build/wwise-audio-web.log'
os.environ['EMSDK_PYTHON'] = sys.executable
with log_path.open('wb') as log:
    result = subprocess.run([sys.executable, manifest['Compiler'], *manifest['CommonArgs'], '-c', str(source), '-o', str(object_path)],
                            stdout=log, stderr=subprocess.STDOUT)
status = dict(Compiled=result.returncode == 0, Source=str(source), SourceSHA256=hashlib.sha256(source.read_bytes()).hexdigest(),
              Object=str(object_path), Log=str(log_path), CompileArguments=manifest['CommonArgs'],
              OriginalMixerSource='CAkSinkBase::Consume -> AkMixer::ApplyGainAndInterleave',
              Backend='Real Web Audio AudioBufferSourceNode scheduling',
              OutputChannels=2, PcmPlaybackVerified=False, AudioStartupVerified=False,
              GameRunning=False, OriginalMenusVerified=False)
(root / 'build/browser-audio-result.json').write_text(json.dumps(status, indent=2))
if result.returncode:
    print('\n'.join(line for line in log_path.read_text(errors='replace').splitlines() if 'error:' in line))
else:
    print('Browser output layer compiled against original Wwise mixer; runtime remains unverified')
sys.exit(result.returncode)
