#!/usr/bin/env python3
"""[w32-menupup3] mem=ios: iPhones were killed (memory) right after picking a difficulty: the deferred character-creation
puppets loaded on the Difficulty scenario. Now they load from LifePath on (loading at BodyType itself left that screen empty). Run after patch-menupuppets2-w32.py."""
from pathlib import Path
p = Path(r'D:\2077\build-w32\src-w32\dev\src\common\gameui\src\basemenugamecontroller.cpp')
raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
s = raw.replace('\r\n', '\n')
if '[w32-menupup3]' in s: raise SystemExit('already')
edits = [
 ('''( std::strstr( cur, "Difficulty" ) || std::strstr( cur, "LifePath" ) || std::strstr( cur, "BodyType" ) || std::strstr( cur, "Character" ) || std::strstr( cur, "NewGame" ) || std::strstr( cur, "Customiz" ) )''',
  '''( std::strstr( cur, "LifePath" ) || std::strstr( cur, "BodyType" ) || std::strstr( cur, "Character" ) || std::strstr( cur, "Customiz" ) ) /* [w32-menupup3] */'''),
 ('''				if (m_entitySystem)
					m_entitySystem->ScheduleEntitySetLOD(puppet, ent::EntityLOD::ForcedHighestQuality);''',
  '''				if (m_entitySystem) // [w32-menupup3] (normal LODs hid the BodyType puppets; reverted)
					m_entitySystem->ScheduleEntitySetLOD(puppet, ent::EntityLOD::ForcedHighestQuality);'''),
]
for a, b in edits:
    assert s.count(a) == 1, (a[:80], s.count(a)); s = s.replace(a, b)
p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)
