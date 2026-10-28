import hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
manifest=json.loads((root/'build/modules/network/compile-jobs.json').read_text(encoding='utf-8-sig'))
args=[a for a in manifest['CommonArgs'] if a!='-DRED_MODULE_network']
os.environ['EMSDK_PYTHON']=sys.executable
units=[]
for name in ('galaxy-state-web','galaxy-user-web','galaxy-telemetry-web','galaxy-network-web'):
 source=root/f'src/{name}.cpp';obj=root/f'build/objects/{name}.o';log=root/f'build/{name}.log'
 with log.open('wb') as f:result=subprocess.run([sys.executable,manifest['Compiler'],*args,'-c',str(source),'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
 units.append({'Compiled':result.returncode==0,'Object':str(obj),'Source':str(source),'SourceSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),'Log':str(log)})
 if result.returncode:print(log.read_text(errors='replace'))
complete=all(unit['Compiled'] for unit in units)
(root/'build/galaxy-boundary-result.json').write_text(json.dumps({'Compiled':complete,'Object':units[0]['Object'],'Objects':[unit['Object'] for unit in units],
 'Units':units,'Args':args,'RuntimeVerified':False,'AuthenticationImplemented':False,'TelemetryNetworkDeliveryImplemented':False,
 'UserInterfaceImplemented':complete,'TelemetryParameterStateImplemented':complete,'NetworkingServiceImplemented':False,'MatchmakingServiceImplemented':False,
 'Scope':'Browser Galaxy unavailable-service interfaces, explicit failed authentication, matchmaking and NAT callbacks, bounded typed telemetry parameters, genuine random visit IDs and failed-send callbacks. No fabricated account, lobby, packet delivery or sent-event success.'},indent=2))
print('Galaxy interface compilation:',complete)
sys.exit(0 if complete else 1)
