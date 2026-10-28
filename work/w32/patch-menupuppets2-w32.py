#!/usr/bin/env python3
"""[w32-menupup2] mem=ios: the pre-game menu's two character-creation puppets (character_creation_female/male) are
NOT created at the main menu; they load on the first menu scenario change towards new game / character creation.
?menuPuppets=1 restores the eager load. Run after patch-menupuppets-w32.py."""
from pathlib import Path
p = Path(r'D:\2077\build-w32\src-w32\dev\src\common\gameui\src\basemenugamecontroller.cpp')
raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
s = raw.replace('\r\n', '\n')
if '[w32-menupup2]' in s: raise SystemExit('already')
edits = [
 ('''                    static const int cpSkip = ::CpWebMemIos() && MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'menuPuppets' ) === '0' ? 1 : 0; } catch ( e ) { return 0; } } );
                    if ( cpSkip ) { std::fprintf(stderr,"CP2077 w32 menu puppets: skipped (?menuPuppets=0)\\n"); return; }''',
  '''                    static const int cpEager = !::CpWebMemIos() || MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'menuPuppets' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
                    const bool cpPreGame = GetClass() && std::strstr( GetClass()->GetName().AsChar(), "PreGameMenu" ) != nullptr; // [w32-menupup2]
                    if ( !cpEager && cpPreGame ) { s_cpDeferredPuppets = this; std::fprintf(stderr,"CP2077 w32 menu puppets: deferred until new game / character creation\\n"); return; }'''),
 ('''		void BaseMenuGameController::OnScenarioChanged( CName prevScenario, CName currScenario )
		{''', '''		void BaseMenuGameController::OnScenarioChanged( CName prevScenario, CName currScenario )
		{
#ifdef __EMSCRIPTEN__
			{ // [w32-menupup2] deferred pre-game puppets load when the player heads into new game / character creation
				const char* cur = currScenario.AsChar();
				std::fprintf( stderr, "CP2077 w32 menu scenario: %s -> %s\\n", prevScenario.AsChar(), cur );
				if ( s_cpDeferredPuppets == this && cur && ( std::strstr( cur, "Difficulty" ) || std::strstr( cur, "LifePath" ) || std::strstr( cur, "BodyType" ) || std::strstr( cur, "Character" ) || std::strstr( cur, "NewGame" ) || std::strstr( cur, "Customiz" ) ) )
				{
					s_cpDeferredPuppets = nullptr;
					std::fprintf( stderr, "CP2077 w32 menu puppets: loading deferred character-creation puppets\\n" );
					InitializePuppets();
				}
			}
#endif'''),
 ('''extern "C" int CpWebMemIos(); // [w32-menupup]
''', '''extern "C" int CpWebMemIos(); // [w32-menupup]
#include <cstring>
namespace game { namespace ui { class BaseMenuGameController; } }
static game::ui::BaseMenuGameController* s_cpDeferredPuppets = nullptr; // [w32-menupup2]
'''),
 ('''			QueueEvent( CreateHandle<ink::MenuInstance_MenuInstanceListener>( false, m_menuInstanceListener ) );
			m_menuInstanceListener.Reset();''', '''			if ( s_cpDeferredPuppets == this ) s_cpDeferredPuppets = nullptr; // [w32-menupup2]
			QueueEvent( CreateHandle<ink::MenuInstance_MenuInstanceListener>( false, m_menuInstanceListener ) );
			m_menuInstanceListener.Reset();'''),
]
for a, b in edits:
    assert s.count(a) == 1, (a[:80], s.count(a)); s = s.replace(a, b)
p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)
