from pathlib import Path
import json,re,statistics
r=Path('D:/2077/build/live-telemetry')
records=[]
for line in (r/'history.jsonl').read_text().splitlines():
    try:records.append(json.loads(line))
    except json.JSONDecodeError:continue # A concurrently appended last line may be incomplete.
assert records
session=records[-1]['sessionId'];rows=[x for x in records if x.get('sessionId')==session]
selected=[]
for previous,current in zip(rows,rows[1:]):
    dt=current['serverReceivedAt']-previous['serverReceivedAt']
    if not 0<dt<=3:continue
    asset,current_frames=current.get('assets') or {},(current.get('presentation') or {}).get('validatedFrames',0)
    prior_asset=previous.get('assets') or {};prior_frames=(previous.get('presentation') or {}).get('validatedFrames',0)
    if not current_frames or current_frames<=prior_frames:continue
    if asset.get('bytesFetched',0)!=prior_asset.get('bytesFetched',0):continue
    match=re.search(r'previous commands ([\d.]+)',current.get('overlay',''))
    selected.append({'at':current['at'],'fps':(current_frames-prior_frames)/dt,
                     'previousCommandsMs':float(match[1]) if match else None})
last=selected[-30:]
values=[row['fps'] for row in last]
waits=[row['previousCommandsMs'] for row in last if row['previousCommandsMs'] is not None]
report={'Session':session,'LatestAt':rows[-1]['at'],'Build':rows[-1].get('build'),
        'Scope':'Recent advancing-frame intervals with zero newly fetched asset bytes; screen identity not yet logged.',
        'Samples':len(last),'FPSMedian':statistics.median(values) if values else None,
        'PreviousCommandsMedianMs':statistics.median(waits) if waits else None,
        'LongLoggerGapsSeconds':[round(b['serverReceivedAt']-a['serverReceivedAt'],3) for a,b in zip(rows,rows[1:]) if b['serverReceivedAt']-a['serverReceivedAt']>3],
        'LatestStage':rows[-1].get('loading'),'FullGameVerified':False}
(r/'menu-performance-summary.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
