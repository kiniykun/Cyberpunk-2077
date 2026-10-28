#include "build.h"
#include <cstdio>
#include "runtimeSystemPhysics.h"
#include "clothMeshNodeInstance.h"

#include "../../world/include/runtimeSystemEnvironment.h"
#include "../../world/include/gameInstance.h"

#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../redJobs2/include/jobBuilder.h"

#include "../../world/include/cameraSystemInterface.h"

#include "../../worldEntities/include/entityContext.h"
#include "../../worldEntities/include/entityComponent.h"
#include "../../worldEntities/include/cameraInterface.h"

#include "../../worldStreaming/include/runtimeSystemWorldStreaming.h"
#include "../../worldStreaming/include/worldStreamingWorld.h"

#include "../../resourceMaterial/include/material.h"
#include "../../resourceMesh/include/mesh.h"
#include "../../engine/include/timeDilationInterface.h"

#include "../../physics/include/physicsNestedScene.h"
#include "../../physics/include/physicsGeometryCache.h"
// CP2077 web [sandbox-floor]
#include "../../physics/include/physicsCollider.h"
#include "../../physics/include/physicsProxy.h"
#include "../../physics/include/physicsSceneStructs.h"
#include "../../physics/include/physicsFilterData.h"
#include <atomic>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
// CP2077 web [citycol] round 2: direct PhysX checks of the world-collision actors under the player
// (compiled with -I physics/include and -I physics/src added in build/research/o2-jobs/worldPhysics-citycol.json)
#include "PxPhysicsAPI.h"
#include "geometry/PxGeometryQuery.h"
#include "extensions/PxShapeExt.h"
#include "../../physics/include/physXScene.h"
#include "../../physics/src/physicsIDLookup.h"
#include "../../physics/src/physXProxyWorldCollision.h"
// CP2077 web [citycol] round 3: always-loaded geometry cache sector verification / reload
#include "../../physics/include/physicsGeometryCache.h"
#include "../../physics/include/physXNatives.h"
#include "../../redJobs2/include/jobBuilder.h"

#include "../../renderData/include/renderPublicFunctions.h"
#include "../../renderData/include/renderFrameInfo.h"

#ifndef RED_CONFIGURATION_FINAL
#include "../../engine/include/tickInfo.h"
#include "../../input/include/inputContext.h"
#include "../../physics/include/physicsConfigVars.h"
#include "../../physics/include/physicsCollisionFilterResource.h"
#include "../../physics/include/physXNatives.h"
#include "../../physics/include/physicsMemory.h"
#include "../../physics/include/physicsQueriesDebug.h"
#include "../../physics/include/physicsClothDebug.h"
#include "../../redMemory/include/poolUtils.h"
#include "../../renderData/include/drawer.h"
#include "../../renderDraw/include/textDrawer.h"
#endif

#ifdef PHYSICS_DEBUG
#pragma optimize("", off)
#endif

#ifdef RED_USE_IMGUI
#include "../../imgui/include/redImGui.h"
#endif

RTTI_BEGIN_TYPE_IN_NAMESPACE( RuntimeSystemPhysics, world );
	RTTI_PARENT_TYPE( world::IRuntimeSystem );
RTTI_END_TYPE();

namespace Config
{
	TConfigVar< Bool > cvHidePhysicsOverflowWarnings( "Editor/RenderDebug", "HidePhysicsOverflowWarnings", false, eConsoleVarFlag_Save );
	TConfigVar< Float > cvPhysicsHighUsageThreshold( "Editor/RenderDebug", "PhysicsHighUsageThreshold", 0.8f, 0.0f, 1.0f, eConsoleVarFlag_Developer );
	TConfigVar< Int32 > cvPhysicsOverflowWarningTextSpeed( "Editor/RenderDebug", "PhysicsOverflowWarningTextSpeed", 3, 1, 5, eConsoleVarFlag_Developer );
}

namespace
{
	static const Vector3 c_validPlayerExtMounted = Vector3( 4.0f, 4.0f, 2.0f );
	static const Vector3 c_validPlayerExtAfoot = Vector3( 1.5f, 1.5f, 2.0f );
}

// CP2077 web [citycol]: spawn/teleport ground gate + static scene-query rebuild + collision probe.
// Watson / Little China (nomadfree startPos -438.98,414.68,23.3): V fell through the street right after the
// loading screen; the PSM out-of-world failsafe (z < -100, vz < -50) teleported V to a never-recorded safe
// position (the origin), the >= 64 m jump made SessionActive reload (SessionLoading), forever. Round 1 (W1)
// showed: 36 LOD0 world-collision proxies Attached + in scene, all 853 triangle meshes of cache sector 41
// loaded, yet every static raycast under V misses -> the actors exist but scene queries do not see them.
// Round 2:
//  * SQ rebuild (default ON, ?citycolSqRebuild=0 off): when the gate is armed after a load/teleport and the
//    ground ray misses, request physics::ForceSpatialTreeRebuild( static ) (the same call the loading screen's
//    "Rebuilding physics spatial tree" stage makes) on the next frame's PreBuckets (no simulation in flight).
//    Once at the first non-loading check of every arm, then every 2 s while missing, max 5 per arm.
//  * Gate (default ON, ?citycol=0 off): holds the engine's "area around player valid" flag FALSE (the
//    engine's own on-foot freeze) only while (a) loading, or (b) the engine flag itself is false, or (c) an SQ
//    rebuild is pending / ran < 1.5 s ago (max 3 s per arm). ?citycolHold=strict also holds while the engine
//    flag is true. Every hold ends at most ?citycolHoldMax=N s (default 10) after loading ends.
//  * Probe ('CP2077 citycol probe' + 'CP2077 citycol px'): node states, cache sector, RED rays, plus direct
//    PhysX checks of the attached world-collision actors under V (actor scene, shape flags/filter words,
//    per-shape PxGeometryQuery::raycast, raw PxScene::raycast without RED filtering).
// Logs: 'CP2077 citycol switches', 'CP2077 citycol gate: ARMED/HOLD/RELEASED/TIMEOUT', 'CP2077 citycol sqrebuild',
// 'CP2077 citycol probe', 'CP2077 citycol px', 'CP2077 citycol valid'.
// Round 3 ROOT CAUSE (offline decode of the cooked nodes + 03_night_city.geometry_cache): 764 of the 1041 mesh keys
// used by the collision nodes within 80 m of the Little China spawn (incl. both street tiles under V) are stored in
// the cache's ALWAYS-LOADED sector (9174 keys, 13.5 MB DDB), not in their node's cache sector 41. In game only the
// own-sector meshes became shapes (tri224 cvx16 box504 = boxes + ~27% of the meshes): the always-loaded geometry was
// absent from m_loadedGeometry when the nodes were created, and CreateShape silently drops a mesh shape whose
// geometry is missing (never retried). Badlands ground (key 0aa343da19465771) is in its own sector 291 -> works.
// Fix (default ON, ?citycolFix=0 = engine behaviour): 'CP2077 citycol al' verifies the always-loaded sector once it
// reports Loaded; node proxies are not initialized until it is complete (max 25 s after world load); if keys are
// missing (or the sector never reaches Loaded within 30 s) its DDB is re-issued and the missing meshes created exactly
// like LoadAlwaysLoadedSector does (one reference each, so FreeAlwaysLoadedSector stays balanced); nodes initialized
// before the sector was complete are re-initialized once through the native out-of-range path.
namespace world
{
	namespace cpweb_citycol
	{
		static std::atomic< Int32 > s_fixOn{ 1 };          // round 3: always-loaded cache sector fix (?citycolFix=0 off)
		static std::atomic< Int32 > s_alReady{ 0 };        // 1 = always-loaded geometry complete (or given up) for this world
		static std::atomic< Int32 > s_alReload{ 0 };       // 0 none, 1 in flight, 2 done, 3 failed
		static std::atomic< Int32 > s_reinitMask{ 0 };     // LOD bits whose nodes must be re-initialized once
		static std::atomic< Uint32 > s_initsNotReady{ 0 }; // node inits dispatched while the sector was incomplete
		static std::atomic< Int32 > s_gateTimedOut{ 0 };
		static double s_worldLoadedAt = 0.0;
		static double s_firstInitAt = 0.0;
		static Int32 s_lastAlState = -1;
		static std::atomic< Int32 > s_gateOn{ 1 };
		static std::atomic< Int32 > s_probeOn{ 1 };
		static std::atomic< Int32 > s_holdMaxSec{ 10 };
		static std::atomic< Int32 > s_strict{ 0 };
		static std::atomic< Int32 > s_sqRebuildOn{ 1 };
		static std::atomic< Int32 > s_inLoadingTick{ 0 };
		static std::atomic< Int32 > s_switchesRead{ 0 };
		static std::atomic< Int32 > s_rebuildRequested{ 0 };
		static std::atomic< Int32 > s_rebuildsDone{ 0 };
		static const char* s_rebuildReason = "";
		static Bool s_armed = false;
		static Bool s_lastRefValid = false;
		static Vector3 s_lastRef = Vector3::ZEROS();
		static double s_armedAt = 0.0;
		static double s_activeSince = 0.0;      // first non-loading check of this arm (0 = still loading)
		static double s_lastRebuildAt = 0.0;     // last executed SQ rebuild (any arm)
		static double s_lastRequestAt = 0.0;
		static Uint32 s_armRebuilds = 0;
		static double s_rebuildHoldMs = 0.0;     // time held for rebuilds in this arm
		static double s_lastCheckAt = 0.0;
		static Uint32 s_hits = 0;
		static Uint32 s_holds = 0;
		static Uint32 s_arms = 0;
		static Uint32 s_gateLines = 0;
		static const char* s_armReason = "";
		static double s_probeUntil = 0.0;
		static double s_nextProbe = 0.0;
		static Uint32 s_probeLines = 0;
		static Int32 s_lastValid = -1;
		static Uint32 s_validLines = 0;

		static double NowMs()
		{
#ifdef __EMSCRIPTEN__
			return emscripten_get_now();
#else
			return 0.0;
#endif
		}

		// Called from RuntimeSystemPhysics::OnStreamingWorldLoaded (same thread/place as the [sandbox-floor] switch read).
		static void ReadSwitches()
		{
			if ( s_switchesRead.exchange( 1 ) )
				return;
#ifdef __EMSCRIPTEN__
			const Int32 gate = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'citycol' ); return ( v === '0' || v === 'false' || v === 'off' ) ? 0 : 1; } catch ( e ) { return 1; }
			} );
			const Int32 probe = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'citycolprobe' ); return ( v === '0' || v === 'false' || v === 'off' ) ? 0 : 1; } catch ( e ) { return 1; }
			} );
			const Int32 holdMax = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'citycolHoldMax' ); if ( v === null ) return 10; const n = parseInt( v, 10 ); return ( isFinite( n ) && n >= 0 ) ? n : 10; } catch ( e ) { return 10; }
			} );
			const Int32 strict = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'citycolHold' ); return v === 'strict' ? 1 : 0; } catch ( e ) { return 0; }
			} );
			const Int32 sq = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'citycolSqRebuild' ); return ( v === '0' || v === 'false' || v === 'off' ) ? 0 : 1; } catch ( e ) { return 1; }
			} );
			const Int32 fix = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'citycolFix' ); return ( v === '0' || v === 'false' || v === 'off' ) ? 0 : 1; } catch ( e ) { return 1; }
			} );
			s_fixOn.store( fix );
			s_gateOn.store( gate );
			s_probeOn.store( probe );
			s_holdMaxSec.store( holdMax );
			s_strict.store( strict );
			s_sqRebuildOn.store( sq );
#endif
			std::fprintf( stderr, "CP2077 citycol switches: fix=%d gate=%d hold=%s holdMax=%d s sqRebuild=%d probe=%d (?citycolFix=0, ?citycol=0, ?citycolHold=strict, ?citycolHoldMax=N, ?citycolSqRebuild=0, ?citycolprobe=0)\n",
				Int32( s_fixOn.load() ), Int32( s_gateOn.load() ), s_strict.load() ? "strict" : "engine", Int32( s_holdMaxSec.load() ), Int32( s_sqRebuildOn.load() ), Int32( s_probeOn.load() ) );
		}

		static void Arm( const char* reason, const Vector3& ref )
		{
			s_armed = true;
			s_armedAt = NowMs();
			s_activeSince = 0.0;
			s_armRebuilds = 0;
			s_rebuildHoldMs = 0.0;
			s_lastCheckAt = 0.0;
			s_hits = 0;
			s_holds = 0;
			++s_arms;
			s_armReason = reason;
			s_probeUntil = s_armedAt + 120000.0;
			s_nextProbe = 0.0;
			if ( s_gateLines < 400 )
			{
				++s_gateLines;
				std::fprintf( stderr, "CP2077 citycol gate: ARMED #%u (%s) ref=(%.2f, %.2f, %.2f) gate=%d\n", s_arms, reason, ref.X, ref.Y, ref.Z, Int32( s_gateOn.load() ) );
			}
		}

		static void RequestRebuild( const char* reason, double now )
		{
			if ( !s_sqRebuildOn.load() || s_armRebuilds >= 5 || s_rebuildRequested.load() )
				return;
			if ( s_armRebuilds > 0 && now - s_lastRequestAt < 2000.0 )
				return;
			++s_armRebuilds;
			s_lastRequestAt = now;
			s_rebuildReason = reason;
			s_rebuildRequested.store( 1 );
		}

		// RuntimeSystemPhysics "ApplyWind" PreBuckets tick: game thread, before this frame's simulation is kicked,
		// after the previous one was waited for -> same conditions as the loading screen's spatial tree rebuild.
		static void TickPre( physics::SceneID sceneId )
		{
			if ( !s_rebuildRequested.load() || sceneId == physics::SceneID() )
				return;
			const double t0 = NowMs();
			physics::ForceSpatialTreeRebuild( sceneId, true, false );
			const double t1 = NowMs();
			s_lastRebuildAt = t1;
			const Int32 n = s_rebuildsDone.fetch_add( 1 ) + 1;
			s_rebuildRequested.store( 0 );
			if ( n <= 64 || ( n & ( n - 1 ) ) == 0 )
			{
				std::fprintf( stderr, "CP2077 citycol sqrebuild #%d: static scene-query tree rebuilt in %.1f ms (%s)\n", n, t1 - t0, s_rebuildReason );
			}
		}

		static Bool GroundRay( physics::SceneID scene, const Vector3& ref, Float up, Float down, Uint8 flags, physics::TraceResult& out )
		{
			physics::QueryFilter filter = physics::QueryFilter::ZERO();
			filter.AddGroup( RED_NAME_CONSTEXPR( "Static" ) );
			filter.AddGroup( RED_NAME_CONSTEXPR( "Terrain" ) );
			physics::RaycastParams params = physics::RaycastParams::Create( ref + Vector3( 0.0f, 0.0f, up ), ref - Vector3( 0.0f, 0.0f, down ), filter );
			params.SetFlags( flags );
			return physics::SyncRaycast( scene, params, out ) == physics::SyncResult::Hit;
		}

		static void GateLog( const char* what, const Vector3& ref, Bool hit, Bool loading, Bool engineResult, double now )
		{
			if ( s_gateLines >= 400 )
				return;
			++s_gateLines;
			std::fprintf( stderr, "CP2077 citycol gate: %s #%u ref=(%.2f, %.2f, %.2f) groundRay=%d loading=%d engineValid=%d armed %.1f s, after-load %.1f s, rebuilds %u (%s)\n",
				what, s_holds, ref.X, ref.Y, ref.Z, Int32( hit ), Int32( loading ), Int32( engineResult ), ( now - s_armedAt ) / 1000.0,
				s_activeSince > 0.0 ? ( now - s_activeSince ) / 1000.0 : 0.0, s_armRebuilds, s_armReason );
		}

		// Called at the end of CollisionNodeManager::UpdateIsAreaAroundPlayerValid (m_nodeLock[0] held -> serialized).
		static Bool Gate( physics::SceneID scene, const Vector3& ref, Bool engineResult, Bool mounted )
		{
			if ( s_lastRefValid && ( ref - s_lastRef ).Mag() > 40.0f )
			{
				Arm( "reference position jumped > 40 m (spawn/teleport)", ref );
			}
			s_lastRef = ref;
			s_lastRefValid = true;

			if ( !s_gateOn.load() || !s_armed || mounted )
			{
				return engineResult;
			}

			const Bool loading = s_inLoadingTick.load() != 0;
			physics::TraceResult tr;
			const Bool hit = GroundRay( scene, ref, 2.0f, 4.0f, Uint8( physics::RaycastParams::STATIC_ONLY ), tr );
			const double now = NowMs();
			const double dt = s_lastCheckAt > 0.0 ? ( ( now - s_lastCheckAt ) < 250.0 ? ( now - s_lastCheckAt ) : 250.0 ) : 0.0;
			s_lastCheckAt = now;

			if ( loading )
			{
				++s_holds;
				return false; // nobody walks during loading; keep the first in-game frame frozen too
			}
			if ( s_activeSince == 0.0 )
			{
				s_activeSince = now;
				RequestRebuild( "first in-game check after load/teleport", now ); // make statics streamed in late queryable
			}

			if ( hit )
			{
				if ( ++s_hits >= 3 && !s_rebuildRequested.load() )
				{
					s_armed = false;
					if ( s_gateLines < 400 )
					{
						++s_gateLines;
						std::fprintf( stderr, "CP2077 citycol gate: RELEASED ground z=%.2f proxyType=%d under ref=(%.2f, %.2f, %.2f) after %.1f s (%.1f s after load), %u held checks, %u sq rebuilds, engineValid=%d\n",
							tr.m_worldPosition.Z, Int32( tr.m_proxyType ), ref.X, ref.Y, ref.Z, ( now - s_armedAt ) / 1000.0, ( now - s_activeSince ) / 1000.0, s_holds, s_armRebuilds, Int32( engineResult ) );
					}
					return engineResult;
				}
				++s_holds;
				return false;
			}

			s_hits = 0;
			RequestRebuild( "no ground under the player", now );

			const Int32 holdMax = s_holdMaxSec.load();
			if ( holdMax > 0 && now - s_activeSince > double( holdMax ) * 1000.0 )
			{
				s_armed = false;
				GateLog( "TIMEOUT (no ground within 4 m) -> engine behaviour", ref, hit, loading, engineResult, now );
				return engineResult;
			}

			Bool hold = !engineResult || s_strict.load() != 0;
			if ( !hold && s_rebuildHoldMs < 3000.0 && ( s_rebuildRequested.load() || now - s_lastRebuildAt < 1500.0 ) )
			{
				hold = true; // wait for the pending static SQ rebuild + one re-check before letting V move
				s_rebuildHoldMs += dt;
			}
			if ( !hold )
			{
				static Uint32 s_passLines = 0;
				if ( s_passLines++ < 16 )
					GateLog( "PASS (engine says area valid, no ground ray hit)", ref, hit, loading, engineResult, now );
				return engineResult;
			}
			++s_holds;
			if ( ( s_holds & ( s_holds - 1 ) ) == 0 )
				GateLog( "HOLD", ref, hit, loading, engineResult, now );
			return false;
		}

		static const char* StateName( Uint32 s )
		{
			static const char* const names[] = { "Created", "NodeAttached", "Initialized", "Attaching", "Attached", "Displaced" };
			return s < 6 ? names[ s ] : "?";
		}

		// ---- round 3: always-loaded geometry cache sector ----------------------------------------------------------
		static const char* AlStateName( Int32 s )
		{
			static const char* const names[] = { "Cleared", "Pending", "Loaded", "Abandoned" };
			return ( s >= 0 && s < 4 ) ? names[ s ] : "?";
		}

		static double SinceWorld( double now )
		{
			return s_worldLoadedAt > 0.0 ? ( now - s_worldLoadedAt ) / 1000.0 : -1.0;
		}

		// RuntimeSystemPhysics::OnStreamingWorldLoaded, right after LoadAlwaysLoadedSector was issued.
		static void AlWorldLoaded( physics::GeometryCache* cache )
		{
			s_alReady.store( 0 );
			s_alReload.store( 0 );
			s_reinitMask.store( 0 );
			s_initsNotReady.store( 0 );
			s_gateTimedOut.store( 0 );
			s_worldLoadedAt = NowMs();
			s_firstInitAt = 0.0;
			s_lastAlState = -1;
			if ( cache )
			{
				const auto& al = cache->m_alwaysLoadedSector;
				std::fprintf( stderr, "CP2077 citycol al: world loaded, always-loaded sector keys=%u offset=%u state=%s refCount=%d ddbLoaded=%d fix=%d\n",
					al.m_entrySize, al.m_entryOffset, AlStateName( Int32( al.m_state ) ), Int32( al.m_refCount ), Int32( cache->m_alwaysLoadedSectorDDB.IsLoaded() ), Int32( s_fixOn.load() ) );
			}
		}

		struct AlCounts { Uint32 total = 0, present = 0, nullEntries = 0, triMissing = 0, cvxMissing = 0, hfMissing = 0; };

		static AlCounts AlCount( physics::GeometryCache& c )
		{
			AlCounts r;
			const auto& al = c.m_alwaysLoadedSector;
			if ( Uint64( al.m_entryOffset ) + al.m_entrySize > c.m_sectorGeometries.Size() )
				return r;
			RED_SCOPE_SHARED_LOCK( c.m_lock );
			for ( Uint32 i = 0; i < al.m_entrySize; ++i )
			{
				const physics::GeometryKey& key = c.m_sectorGeometries[ al.m_entryOffset + i ];
				++r.total;
				physx::PxBase* const* p = c.m_loadedGeometry.FindPtr( key.Hash() );
				if ( p && *p )
				{
					++r.present;
					continue;
				}
				if ( p ) ++r.nullEntries;
				const auto t = static_cast< physics::ShapeType >( key.type );
				if ( t == physics::ShapeType::TriangleMesh ) ++r.triMissing;
				else if ( t == physics::ShapeType::ConvexMesh ) ++r.cvxMissing;
				else ++r.hfMissing;
			}
			return r;
		}

		static void AlSetReady( const char* why, const AlCounts& n )
		{
			if ( s_alReady.exchange( 1 ) )
				return;
			const Uint32 early = s_initsNotReady.load();
			const Bool reinit = s_fixOn.load() != 0 && ( early > 0 || s_alReload.load() == 2 );
			if ( reinit )
				s_reinitMask.store( 0xF );
			std::fprintf( stderr, "CP2077 citycol al: READY (%s) present=%u/%u missing tri=%u cvx=%u other=%u null=%u at +%.1f s after world load; node inits before ready=%u (first init +%.1f s) gateTimedOut=%d -> reinit=%d\n",
				why, n.present, n.total, n.triMissing, n.cvxMissing, n.hfMissing, n.nullEntries, SinceWorld( NowMs() ), early,
				s_firstInitAt > 0.0 ? ( s_firstInitAt - s_worldLoadedAt ) / 1000.0 : -1.0, Int32( s_gateTimedOut.load() ), Int32( reinit ) );
		}

		// Re-issue the always-loaded DDB and create every missing mesh exactly like GeometryCache::LoadAlwaysLoadedSector.
		// takeRefsOnExisting: the native job never ran (sector not Loaded) -> also take its reference on keys that other
		// sectors created, so the later FreeAlwaysLoadedSector (one release per key) stays balanced.
		static void AlStartReload( const THandle< physics::GeometryCache >& h, const char* why )
		{
			physics::GeometryCache& c = *h;
			auto& al = c.m_alwaysLoadedSector;
			if ( Uint64( al.m_entryOffset ) + al.m_entrySize > c.m_sectorGeometries.Size() || Uint64( al.m_entryOffset ) + al.m_entrySize > c.m_sectorCacheEntries.Size() )
			{
				std::fprintf( stderr, "CP2077 citycol al: reload impossible (span out of range)\n" );
				s_alReload.store( 3 );
				return;
			}
			s_alReload.store( 1 );
			const Bool takeRefsOnExisting = al.m_state != physics::Loaded;
			auto sectorKeys = red::MakeArraySpan( c.m_sectorGeometries ).SubSpan( al.m_entryOffset, al.m_entrySize );
			auto sectorCacheEntries = red::MakeArraySpan( c.m_sectorCacheEntries ).SubSpan( al.m_entryOffset, al.m_entrySize );
			std::fprintf( stderr, "CP2077 citycol al: RELOAD start (%s) state=%s refCount=%d ddbLoaded=%d takeRefsOnExisting=%d at +%.1f s\n",
				why, AlStateName( Int32( al.m_state ) ), Int32( al.m_refCount ), Int32( c.m_alwaysLoadedSectorDDB.IsLoaded() ), Int32( takeRefsOnExisting ), SinceWorld( NowMs() ) );

			job::Counter result{ job::Priority::Latent };
			auto token = c.m_alwaysLoadedSectorDDB.IssueLoadingJobs( io::eAsyncPriority_COLLISION_CRITICAL );
			result += token.GetWaitCounter();
			WeakHandle< physics::GeometryCache > cacheWeak( h );
			const double t0 = NowMs();
			job::Builder builder{ job::Priority::Latent };
			builder.DispatchWait( result );
			builder.DispatchJob( "CP2077 citycol AlwaysLoaded reload", [ sectorKeys, sectorCacheEntries, cacheWeak, takeRefsOnExisting, t0 ]( const job::RunContext& )
			{
				auto cache = cacheWeak.ToHandle();
				if ( !cache )
				{
					s_alReload.store( 3 );
					return;
				}
				auto& buffer = cache->m_alwaysLoadedSectorDDB;
				red::BlobSpan all = buffer.Data().GetSpan();
				Uint32 created = 0, failed = 0, had = 0, refsTaken = 0, outOfRange = 0, empty = 0, nullFixed = 0;
				for ( auto i : sectorKeys.Indices() )
				{
					const auto& key = sectorKeys[ i ];
					const auto type = static_cast< physics::ShapeType >( key.type );
					RED_SCOPE_LOCK( cache->m_lock );
					physx::PxBase** existing = cache->m_loadedGeometry.FindPtr( key.Hash() );
					if ( existing && *existing )
					{
						++had;
						if ( takeRefsOnExisting )
						{
							if ( type == physics::ShapeType::TriangleMesh ) { if ( auto* m = ( *existing )->is< physx::PxTriangleMesh >() ) { m->acquireReference(); ++refsTaken; } }
							else if ( type == physics::ShapeType::ConvexMesh ) { if ( auto* m = ( *existing )->is< physx::PxConvexMesh >() ) { m->acquireReference(); ++refsTaken; } }
							else if ( type == physics::ShapeType::Heightfield ) { if ( auto* m = ( *existing )->is< physx::PxHeightField >() ) { m->acquireReference(); ++refsTaken; } }
						}
						continue;
					}
					const auto& entry = sectorCacheEntries[ i ];
					if ( !entry.m_entrySize )
					{
						++empty;
						continue;
					}
					if ( Uint64( entry.m_entryOffset ) + entry.m_entrySize > all.Size() )
					{
						++outOfRange;
						continue;
					}
					red::BlobSpan span = all.Range( entry.m_entryOffset, entry.m_entrySize );
					physx::PxBase* geom = nullptr;
					if ( type == physics::ShapeType::TriangleMesh )
					{
						if ( auto* mesh = physics::CreatePxTriangleMesh( span ) ) { mesh->acquireReference(); geom = mesh; }
					}
					else if ( type == physics::ShapeType::ConvexMesh )
					{
						if ( auto* mesh = physics::CreatePxConvexMesh( span ) ) { mesh->acquireReference(); geom = mesh; }
					}
					else if ( type == physics::ShapeType::Heightfield )
					{
						if ( auto* mesh = physics::CreatePxHeightfield( span ) ) { mesh->acquireReference(); geom = mesh; }
					}
					if ( !geom )
					{
						++failed;
						continue;
					}
					if ( existing )
					{
						*existing = geom; // a null entry left by a failed native create
						++nullFixed;
					}
					else
					{
						cache->m_loadedGeometry.Insert( key.Hash(), geom );
					}
					++created;
				}
				if ( takeRefsOnExisting )
					cache->m_alwaysLoadedSector.m_state = physics::Loaded;
				const Uint32 ddbSize = all.Size();
				buffer.Clear();
				std::fprintf( stderr, "CP2077 citycol al: RELOAD done in %.1f ms: ddbSize=%u keys=%u created=%u (nullFixed=%u) alreadyPresent=%u refsTaken=%u failed=%u outOfRange=%u emptyEntries=%u\n",
					NowMs() - t0, ddbSize, Uint32( sectorKeys.Size() ), created, nullFixed, had, refsTaken, failed, outOfRange, empty );
				s_alReload.store( created > 0 || ( failed == 0 && outOfRange == 0 ) ? 2 : 3 );
			} );
		}

		// CollisionNodeManager::TickSectors (m_cacheLock held, m_geometryCache valid).
		static void AlTick( const THandle< physics::GeometryCache >& h )
		{
			physics::GeometryCache& c = *h;
			auto& al = c.m_alwaysLoadedSector;
			const double now = NowMs();
			const Int32 st = Int32( al.m_state );
			if ( st != s_lastAlState )
			{
				std::fprintf( stderr, "CP2077 citycol al: state %s -> %s refCount=%d keys=%u ddbLoaded=%d at +%.1f s after world load\n",
					AlStateName( s_lastAlState ), AlStateName( st ), Int32( al.m_refCount ), al.m_entrySize, Int32( c.m_alwaysLoadedSectorDDB.IsLoaded() ), SinceWorld( now ) );
				s_lastAlState = st;
			}
			if ( s_alReady.load() )
				return;
			const Int32 rl = s_alReload.load();
			if ( rl == 1 )
				return;
			if ( al.m_entrySize == 0 )
			{
				AlSetReady( "no always-loaded geometry", AlCounts() );
				return;
			}
			if ( st == physics::Loaded || rl >= 2 )
			{
				const AlCounts n = AlCount( c );
				const Uint32 missing = n.total - n.present;
				if ( missing == 0 && n.total > 0 )
					AlSetReady( rl == 2 ? "complete after reload" : "verified complete", n );
				else if ( s_fixOn.load() && rl == 0 )
				{
					std::fprintf( stderr, "CP2077 citycol al: INCOMPLETE present=%u/%u (missing tri=%u cvx=%u other=%u null=%u) -> reload\n",
						n.present, n.total, n.triMissing, n.cvxMissing, n.hfMissing, n.nullEntries );
					AlStartReload( h, "sector Loaded but keys missing" );
				}
				else
					AlSetReady( rl >= 2 ? "reload incomplete, giving up" : "INCOMPLETE (fix off)", n );
				return;
			}
			if ( s_fixOn.load() && rl == 0 && s_worldLoadedAt > 0.0 && now - s_worldLoadedAt > 30000.0 )
				AlStartReload( h, "sector not Loaded 30 s after world load" );
		}

		// CollisionNodeManager::TickStreaming, right before a node init is dispatched (m_nodeLock[lod] held).
		static Bool InitGateClosed()
		{
			if ( !s_alReady.load() )
			{
				if ( s_fixOn.load() )
				{
					if ( s_worldLoadedAt == 0.0 || NowMs() - s_worldLoadedAt < 25000.0 )
						return true;
					if ( !s_gateTimedOut.exchange( 1 ) )
						std::fprintf( stderr, "CP2077 citycol al: init gate TIMEOUT (always-loaded sector still incomplete 25 s after world load) -> nodes initialize anyway, reinit when it completes\n" );
				}
				s_initsNotReady.fetch_add( 1 );
			}
			if ( s_firstInitAt == 0.0 )
				s_firstInitAt = NowMs();
			return false;
		}

		// CollisionNodeManager::Tick start: re-initialize (native out-of-range path) the nodes created before the
		// always-loaded sector was complete. Waits up to 5 s per LOD for in-flight inits to finish first.
		static void ReinitPass( CollisionNodeManager& m )
		{
			const Int32 mask = s_reinitMask.load();
			if ( !mask )
				return;
			static double s_waitSince[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
			const double now = NowMs();
			for ( Uint32 lod = 0; lod < CollisionNodeManager::c_maxLOD && lod < 4; ++lod )
			{
				if ( !( mask & ( 1 << lod ) ) )
					continue;
				RED_SCOPE_LOCK( m.m_nodeLock[ lod ] );
				Uint32 busy = 0;
				for ( auto it = m.m_nodesMap[ lod ].Begin(); it != m.m_nodesMap[ lod ].End(); ++it )
				{
					auto& e = it.Value();
					if ( !e.IsFlagSet( ImmediateAttach ) && ( e.IsFlagSet( Initializing ) || e.m_state == Attaching ) )
						++busy;
				}
				if ( s_waitSince[ lod ] == 0.0 )
					s_waitSince[ lod ] = now;
				if ( busy > 0 && now - s_waitSince[ lod ] < 5000.0 )
					continue;
				Uint32 reset = 0, attachedReset = 0;
				for ( auto it = m.m_nodesMap[ lod ].Begin(); it != m.m_nodesMap[ lod ].End(); ++it )
				{
					auto& e = it.Value();
					const CollisionNodeId id = it.Key();
					if ( e.IsFlagSet( ImmediateAttach ) || e.IsFlagSet( Initializing ) )
						continue;
					if ( e.m_state != Attached && e.m_state != Initialized )
						continue;
					if ( physics::IsAlive( e.m_proxyId ) )
					{
						if ( e.m_state == Attached )
						{
							physics::DetachProxy( m.m_sceneId, e.m_proxyId );
							++attachedReset;
						}
						physics::DestroyProxy( e.m_proxyId );
					}
					e.m_proxyId = physics::ProxyID();
					e.m_state = NodeAttached;
					m.m_inRange[ lod ].Remove( id );
					m.m_inRangeInitialized[ lod ].Remove( id );
					++reset;
				}
				s_reinitMask.fetch_and( ~( 1 << lod ) );
				s_waitSince[ lod ] = 0.0;
				std::fprintf( stderr, "CP2077 citycol al: REINIT lod%u: %u nodes reset (%u were attached), %u still busy, at +%.1f s after world load\n",
					lod, reset, attachedReset, busy, SinceWorld( now ) );
			}
		}

		// Direct PhysX view of the world-collision proxies under the player: are their actors in this PxScene, do their
		// shapes carry eSCENE_QUERY_SHAPE / filter words, does a per-shape PxGeometryQuery raycast (no scene query
		// structure involved) hit them, and does a raw PxScene::raycast (scene query structure, no RED filtering)?
		static void PxProbe( physics::SceneID sceneId, const Vector3& ref, const physics::ProxyID* ids, Uint32 count )
		{
			physics::PhysXScene* scene = physics::g_sceneLookup ? physics::g_sceneLookup->GetScene( sceneId ) : nullptr;
			physx::PxScene* pxScene = scene ? scene->GetPxScene() : nullptr;
			if ( !pxScene )
			{
				std::fprintf( stderr, "CP2077 citycol px: no PhysX scene\n" );
				return;
			}
			const Vector3 shift = scene->GetOriginShift();
			const Vector3 o = ref - shift; // PhysX space
			const physx::PxVec3 origin( o.X, o.Y, o.Z + 50.0f );
			const physx::PxVec3 down( 0.0f, 0.0f, -1.0f );
			const physx::PxReal dist = 100.0f;

			Uint32 actors = 0, actorsInThisScene = 0, actorsNoScene = 0, actorsOtherScene = 0, shapes = 0, tri = 0, convex = 0, box = 0, hf = 0, other = 0;
			Uint32 sqShapes = 0, simShapes = 0, zeroQueryWords = 0, boundsUnder = 0, shapeHits = 0, triHits = 0, shapelessActors = 0;
			Float bestZ = -1.0e9f;
			Int32 bestType = -1;
			physx::PxFilterData firstTriQuery;
			Bool haveFirstTri = false;
			Float firstTriBoundsMin = 0.0f, firstTriBoundsMax = 0.0f;
			for ( Uint32 p = 0; p < count; ++p )
			{
				physics::ProxySharedPtr sp = physics::g_proxyLookup ? physics::g_proxyLookup->GetProxy( ids[ p ] ) : physics::ProxySharedPtr();
				if ( !sp || sp->m_type != physics::ProxyType::WorldCollision )
					continue;
				const physics::ProxyWorldCollision* wc = static_cast< const physics::ProxyWorldCollision* >( sp.Get() );
				const Uint32 n = wc->GetNumPxRigidActors();
				for ( Uint32 a = 0; a < n; ++a )
				{
					physx::PxRigidActor* actor = wc->GetPxRigidActor( a );
					if ( !actor )
						continue;
					++actors;
					physx::PxScene* as = actor->getScene();
					if ( as == pxScene ) ++actorsInThisScene; else if ( !as ) ++actorsNoScene; else ++actorsOtherScene;
					physx::PxShape* sh[ 16 ];
					const physx::PxU32 ns = actor->getNbShapes();
					if ( ns == 0 ) ++shapelessActors;
					for ( physx::PxU32 s0 = 0; s0 < ns; s0 += 16 )
					{
						const physx::PxU32 got = actor->getShapes( sh, 16, s0 );
						for ( physx::PxU32 i = 0; i < got; ++i )
						{
							physx::PxShape* shape = sh[ i ];
							++shapes;
							const physx::PxGeometryType::Enum gt = shape->getGeometryType();
							if ( gt == physx::PxGeometryType::eTRIANGLEMESH ) ++tri;
							else if ( gt == physx::PxGeometryType::eCONVEXMESH ) ++convex;
							else if ( gt == physx::PxGeometryType::eBOX ) ++box;
							else if ( gt == physx::PxGeometryType::eHEIGHTFIELD ) ++hf;
							else ++other;
							const physx::PxShapeFlags f = shape->getFlags();
							if ( f & physx::PxShapeFlag::eSCENE_QUERY_SHAPE ) ++sqShapes;
							if ( f & physx::PxShapeFlag::eSIMULATION_SHAPE ) ++simShapes;
							const physx::PxFilterData q = shape->getQueryFilterData();
							if ( ( q.word0 | q.word1 | q.word2 | q.word3 ) == 0 ) ++zeroQueryWords;
							const physx::PxBounds3 wb = physx::PxShapeExt::getWorldBounds( *shape, *actor );
							const Bool under = wb.minimum.x <= origin.x && origin.x <= wb.maximum.x && wb.minimum.y <= origin.y && origin.y <= wb.maximum.y;
							if ( under ) ++boundsUnder;
							if ( gt == physx::PxGeometryType::eTRIANGLEMESH && under && !haveFirstTri )
							{
								haveFirstTri = true;
								firstTriQuery = q;
								firstTriBoundsMin = wb.minimum.z + shift.Z;
								firstTriBoundsMax = wb.maximum.z + shift.Z;
							}
							if ( under )
							{
								physx::PxRaycastHit hit;
								const physx::PxU32 nh = physx::PxShapeExt::raycast( *shape, *actor, origin, down, dist, physx::PxHitFlag::eDEFAULT, 1, &hit );
								if ( nh > 0 )
								{
									++shapeHits;
									if ( gt == physx::PxGeometryType::eTRIANGLEMESH ) ++triHits;
									if ( hit.position.z > bestZ ) { bestZ = hit.position.z; bestType = Int32( gt ); }
								}
							}
						}
					}
				}
			}

			// raw scene query: static only, no filter words, no RED callback
			physx::PxRaycastBuffer buf;
			const bool sceneHit = pxScene->raycast( origin, down, dist, buf, physx::PxHitFlags( physx::PxHitFlag::eDEFAULT ), physx::PxQueryFilterData( physx::PxQueryFlag::eSTATIC ) );
			Int32 sceneHitType = -1;
			Float sceneHitZ = 0.0f;
			if ( sceneHit && buf.hasBlock )
			{
				sceneHitZ = buf.block.position.z + shift.Z;
				sceneHitType = buf.block.shape ? Int32( buf.block.shape->getGeometryType() ) : -1;
			}
			std::fprintf( stderr, "CP2077 citycol px: shift=(%.0f, %.0f, %.0f) proxies=%u actors=%u inScene=%u noScene=%u otherScene=%u shapes=%u [tri%u cvx%u box%u hf%u oth%u] sqFlag=%u simFlag=%u zeroQueryWords=%u boundsUnderRay=%u | perShapeRay hits=%u triHits=%u bestZ=%.2f geomType=%d | shapelessActors=%u | triUnderRay=%d query=%08x %08x %08x %08x boundsZ=%.2f..%.2f | sceneRaycast(static,unfiltered)=%d z=%.2f geomType=%d | staticActors=%u sqStaticStamp=%u rebuilds=%d\n",
				shift.X, shift.Y, shift.Z, count, actors, actorsInThisScene, actorsNoScene, actorsOtherScene, shapes, tri, convex, box, hf, other, sqShapes, simShapes, zeroQueryWords, boundsUnder,
				shapeHits, triHits, shapeHits ? bestZ + shift.Z : 0.0f, bestType, shapelessActors, Int32( haveFirstTri ),
				haveFirstTri ? firstTriQuery.word0 : 0u, haveFirstTri ? firstTriQuery.word1 : 0u, haveFirstTri ? firstTriQuery.word2 : 0u, haveFirstTri ? firstTriQuery.word3 : 0u, firstTriBoundsMin, firstTriBoundsMax,
				Int32( sceneHit ), sceneHitZ, sceneHitType,
				pxScene->getNbActors( physx::PxActorTypeFlag::eRIGID_STATIC ), pxScene->getSceneQueryStaticTimestamp(), Int32( s_rebuildsDone.load() ) );
		}

		// One probe line set; called from CollisionNodeManager::Tick after UpdateIsAreaAroundPlayerValid (no lock held).
		static void Probe( CollisionNodeManager& m )
		{
			const Vector3 ref = physics::GetReferencePosition( m.m_sceneId ).AsVector3();
			const Bool loading = s_inLoadingTick.load() != 0;
			physics::ProxyID pxIds[ 12 ];
			Uint32 pxCount = 0;

			// collision nodes per LOD: in the engine's validity box (+-1.5,+-1.5,+-2) and in a +-10 m box
			char nodeText[ 4 ][ 200 ];
			for ( Uint32 lod = 0; lod < 3; ++lod )
			{
				Uint32 inValid = 0, nearCount = 0, byState[ 6 ] = {}, blocked = 0, initializing = 0, alive = 0, inScene = 0, subObjects = 0;
				{
					RED_SCOPE_LOCK( m.m_nodeLock[ lod ] );
					const Box validBox( ref - c_validPlayerExtAfoot, ref + c_validPlayerExtAfoot );
					m.m_nodesBVH[ lod ].Query( validBox, [ & ]( const CollisionNodeId& id )
					{
						++inValid;
						if ( lod <= 1 && pxCount < 12 )
						{
							if ( CollisionNodeEntry* e = m.m_nodesMap[ lod ].FindPtr( id ) )
							{
								if ( physics::IsAlive( e->m_proxyId ) ) pxIds[ pxCount++ ] = e->m_proxyId;
							}
						}
						return true;
					} );
					const Vector3 ext10( 10.0f, 10.0f, 10.0f );
					const Box nearBox( ref - ext10, ref + ext10 );
					m.m_nodesBVH[ lod ].Query( nearBox, [ & ]( const CollisionNodeId& id )
					{
						++nearCount;
						if ( CollisionNodeEntry* e = m.m_nodesMap[ lod ].FindPtr( id ) )
						{
							if ( Uint32( e->m_state ) < 6 ) ++byState[ Uint32( e->m_state ) ];
							if ( e->m_sectorId != CollisionNodeManager::c_noSectorId && !m.m_sectorsInRange.Exist( e->m_sectorId ) ) ++blocked;
							if ( e->IsFlagSet( Initializing ) ) ++initializing;
							if ( physics::IsAlive( e->m_proxyId ) )
							{
								++alive;
								if ( physics::IsInScene( e->m_proxyId ) ) ++inScene;
								subObjects += physics::GetNumSubObjects( e->m_proxyId );
							}
						}
						return true;
					} );
				}
				std::snprintf( nodeText[ lod ], sizeof( nodeText[ lod ] ), "lod%u valid=%u near10=%u [C%u NA%u I%u Ag%u A%u D%u] waitSector=%u initializing=%u proxies=%u inScene=%u actors=%u",
					lod, inValid, nearCount, byState[ 0 ], byState[ 1 ], byState[ 2 ], byState[ 3 ], byState[ 4 ], byState[ 5 ], blocked, initializing, alive, inScene, subObjects );
			}

			// geometry cache sector covering the reference position
			Int32 coverIndex = -1, coverState = -1, coverRef = 0;
			Uint32 coverKeys = 0, coverLoaded = 0, coverTri = 0, coverTriLoaded = 0, inRange = 0, loadingSectors = 0;
			{
				RED_SCOPE_LOCK( m.m_cacheLock );
				inRange = m.m_sectorsInRange.Size();
				loadingSectors = m.m_sectorsLoading.Size();
				if ( m.m_geometryCache )
				{
					const Vector3 half( 0.5f, 0.5f, 0.5f );
					m.m_sectorsBVH.Query( Box( ref - half, ref + half ), [ & ]( const SectorID& id ) { if ( coverIndex < 0 ) coverIndex = Int32( id ); return true; } );
					if ( coverIndex >= 0 && Uint32( coverIndex ) < m.m_geometryCache->m_sectorEntries.Size() )
					{
						physics::GeometryCache& cache = *m.m_geometryCache;
						const physics::SectorEntry& se = cache.m_sectorEntries[ coverIndex ];
						coverState = Int32( se.m_state );
						coverRef = Int32( se.m_refCount );
						coverKeys = se.m_entrySize;
						RED_SCOPE_SHARED_LOCK( cache.m_lock );
						for ( Uint32 k = se.m_entryOffset; k < se.m_entryOffset + se.m_entrySize && k < cache.m_sectorGeometries.Size(); ++k )
						{
							const auto& key = cache.m_sectorGeometries[ k ];
							const Bool loaded = cache.m_loadedGeometry.KeyExist( key.Hash() );
							coverLoaded += loaded ? 1 : 0;
							if ( static_cast< physics::ShapeType >( key.type ) == physics::ShapeType::TriangleMesh )
							{
								++coverTri;
								coverTriLoaded += loaded ? 1 : 0;
							}
						}
					}
				}
			}

			// ground rays: from 2 m above (gate ray, 30 m deep), both-sides variant, and from 50 m above
			physics::TraceResult r1, r2, r3;
			const Bool h1 = GroundRay( m.m_sceneId, ref, 2.0f, 30.0f, Uint8( physics::RaycastParams::STATIC_ONLY ), r1 );
			const Bool h2 = GroundRay( m.m_sceneId, ref, 2.0f, 30.0f, Uint8( physics::RaycastParams::STATIC_ONLY | physics::RaycastParams::MESH_BOTH_SIDES ), r2 );
			const Bool h3 = GroundRay( m.m_sceneId, ref, 50.0f, 30.0f, Uint8( physics::RaycastParams::STATIC_ONLY ), r3 );

			std::fprintf( stderr, "CP2077 citycol probe: t=%.1f ref=(%.2f, %.2f, %.2f) loading=%d valid=%d armed=%d mounted=%d | %s | %s | %s | cacheSector=%d state=%d ref=%d keys=%u loaded=%u tri=%u triLoaded=%u sectorsInRange=%u sectorsLoading=%u | ray2m=%d z=%.2f type=%d | rayBoth=%d z=%.2f type=%d | ray50m=%d z=%.2f type=%d\n",
				NowMs() / 1000.0, ref.X, ref.Y, ref.Z, Int32( loading ), Int32( m.m_isValid.GetValue() ), Int32( s_armed ), Int32( m.m_isMounted ),
				nodeText[ 0 ], nodeText[ 1 ], nodeText[ 2 ],
				coverIndex, coverState, coverRef, coverKeys, coverLoaded, coverTri, coverTriLoaded, inRange, loadingSectors,
				Int32( h1 ), h1 ? r1.m_worldPosition.Z : 0.0f, Int32( r1.m_proxyType ),
				Int32( h2 ), h2 ? r2.m_worldPosition.Z : 0.0f, Int32( r2.m_proxyType ),
				Int32( h3 ), h3 ? r3.m_worldPosition.Z : 0.0f, Int32( r3.m_proxyType ) );
			if ( !loading )
			{
				PxProbe( m.m_sceneId, ref, pxIds, pxCount );
			}
		}

		static void AfterUpdate( CollisionNodeManager& m )
		{
			const Int32 valid = m.m_isValid.GetValue() ? 1 : 0;
			if ( valid != s_lastValid )
			{
				if ( s_validLines < 200 )
				{
					++s_validLines;
					const Vector3 ref = physics::GetReferencePosition( m.m_sceneId ).AsVector3();
					std::fprintf( stderr, "CP2077 citycol valid: %d -> %d ref=(%.2f, %.2f, %.2f) loading=%d armed=%d\n",
						s_lastValid, valid, ref.X, ref.Y, ref.Z, Int32( s_inLoadingTick.load() ), Int32( s_armed ) );
				}
				s_lastValid = valid;
			}
			if ( !s_probeOn.load() || s_probeLines >= 400 )
				return;
			const double now = NowMs();
			if ( ( s_armed || now < s_probeUntil ) && now >= s_nextProbe )
			{
				s_nextProbe = now + 1000.0;
				++s_probeLines;
				Probe( m );
			}
		}
	}
}

namespace world
{
	//////////////////////////////////////////////////////////////////////////

	struct QueryVolume
	{
		Sphere sphere;

		Bool Contains( const Box& box ) const
		{
			const Vector3 v = box.CalcCenter() - sphere.GetCenter();
			const Float dist = v.Mag();
			const Float boxRadius = box.CalcExtents().AsVector2().Mag();
			const Float radius = sphere.GetRadius();
			return ( radius >= boxRadius ) && ( dist <= ( radius - boxRadius ) );
		}

		Bool Touches( const Box& box ) const
		{
			return box.IntersectSphere( sphere );
		}
	};

	Box CollisionNodeManager::ComputeBoxForLOD( const Vector3& center, Uint32 lod )
	{
		const Vector3 size[ c_maxLOD ] =
		{
			Vector3( 150.f, 150.f, 100.f ),
			Vector3( 75.f, 75.f, 50.f ),
			Vector3( 24.f, 24.f, 16.f ),
#ifdef RED_PLATFORM_CONSOLE
			Vector3( 32.f, 32.f, 32.f ),
#else
			Vector3( 48.f, 48.f, 48.f ),
#endif
		};

		const Vector3 boxSize = size[ lod ];
		const Vector3 ext = boxSize * 0.5f;
		return Box( center - ext, center + ext );
	}

	CollisionNodeManager::CollisionNodeManager()
	{
		for( Uint32 lod = 0; lod < c_maxLOD; ++lod )
		{
			m_streamingQueryFrameResult[lod].Reserve( 512 );
			m_streamingQueryFrameResultDistances[ lod ].Reserve( 256 );
		}
		m_sectorQueryFrameResult.Reserve( 64 );
		m_additionalObservers.Reserve( 32 );
	}

	void CollisionNodeManager::Tick( Float dt, Bool performQueries, Bool updateState )
	{
		cpweb_citycol::ReinitPass( *this ); // CP2077 web [citycol] r3: re-init nodes built without the always-loaded geometry
		job::Counter sectorsPreload = TickSectors( performQueries );
		TickStreaming( sectorsPreload, performQueries );

		if( updateState )
		{
#ifndef RED_CONFIGURATION_FINAL
			for( SectorID currentLoading : m_sectorsLoading )
			{
				m_sectorLoadingTimeMap[ currentLoading ] += dt;
			}
#endif	

			PC_SCOPE( CollisionNodeManager_UpdateState )
			for( Uint32 lod = 0; lod < c_maxLOD; ++lod )
			{
				RED_SCOPE_LOCK( m_nodeLock[ lod ] );
				for( auto it = m_nodesMap[ lod ].Begin(); it != m_nodesMap[ lod ].End(); ++it )
				{
					auto& nodeEntry = it.Value();
					if( nodeEntry.m_state == Attaching )
					{
						// node scheduled to attach, but not yet attached
						if( physics::IsInScene( nodeEntry.m_proxyId ) )
						{
							nodeEntry.m_state = Attached;
						}
					}
				}
			}
			UpdateIsAreaAroundPlayerValid();
			cpweb_citycol::AfterUpdate( *this ); // CP2077 web [citycol] probe
		}
	}

	job::Counter CollisionNodeManager::TickSectors( Bool performQueries )
	{
		PC_SCOPE( Collision_TickSectors );
		RED_SCOPE_LOCK( m_cacheLock );

		job::Builder builder( job::Priority::Immediate );

		if( !m_geometryCache )
			return builder.ExtractWaitCounter();

		cpweb_citycol::AlTick( m_geometryCache ); // CP2077 web [citycol] r3: verify / reload the always-loaded sector

		auto& scratch = m_sectorQueryFrameResult;
		if( performQueries )
		{
			// Leaves roughly 3sec for the sector to load before it's needed at 150mph
			static const Vector3 sectorExt = Vector3( 225.0f, 225.0f, 100.f );
			const Vector3 referencePostition = physics::GetReferencePosition( m_sceneId ).AsVector3();
			Box sectorBox = Box( referencePostition - sectorExt, referencePostition + sectorExt );
			scratch.Clear();
			m_sectorsBVH.Query( sectorBox, [ &scratch ]( const SectorID& id )
			{
				scratch.Insert( id );
				return true;
			} );
		}

		SectorsIdSet toFree = m_sectorsInRange;
		toFree.Difference( scratch );

		auto weakGeomCache = WeakHandle<physics::GeometryCache>( m_geometryCache );
		for( SectorID id : toFree )
		{
			builder.DispatchJob( "FreeSector", [ id, weakGeomCache ]( const job::RunContext& depCtx )
			{
				if( auto geomCache = weakGeomCache.ToHandle() )
				{
					geomCache->FreeSectorData( id );
				}
			} );

			m_sectorsInRange.Remove( id );
			m_sectorsLoading.Remove( id );
		}

		auto& toCheckLoading = toFree; // toFree is not needed at this point anymore, so we can reuse the memory
		toCheckLoading.Clear(); 
		toCheckLoading.Reserve( m_sectorsLoading.Size() );
		toCheckLoading.Union( m_sectorsLoading );
		for( SectorID id : toCheckLoading )
		{
			if( m_geometryCache->IsSectorLoaded( id ) )
			{
				m_sectorsLoading.Remove( id );
				m_sectorsInRange.Insert( id );
#ifndef RED_CONFIGURATION_FINAL
				m_totalSectorLoads += 1.0f;
				auto loadingTime = m_sectorLoadingTimeMap[ id ];
				m_maxSectorLoadingTime = math::Max( m_maxSectorLoadingTime, loadingTime );
				m_totalSectorLoadingTime += loadingTime;
				m_sectorLoadingTimeMap.Remove( id );
#endif
			}
		}

		auto& toLoad = toFree; // same as above, becomes toLoad 
		toLoad.Clear();
		toLoad.Reserve( scratch.Size() );
		toLoad.Union( scratch );
		toLoad.Difference( m_sectorsInRange );
		toLoad.Difference( m_sectorsLoading );
		for( SectorID id : toLoad )
		{
			m_geometryCache->LoadSectorDataAsync( id );
			m_sectorsLoading.Insert( id );
#ifndef RED_CONFIGURATION_FINAL
			m_sectorLoadingTimeMap[ id ] = 0.f;
#endif
		}
		return builder.ExtractWaitCounter();
	}
	void DispatchInitJob( job::Builder& builder, CollisionNodeEntry* entry, red::WeakPtr<world::CollisionNodeManager> weakThis, Uint32 lod )
	{
		entry->m_initFunction(builder);
		builder.DispatchJob( "OnInitFuncCompleted", [ weakThis, lod ]( const job::RunContext& ctx )
		{
			if( auto strongThis = weakThis.Lock() )
			{
				strongThis->m_initInProgress[ lod ].Decrement();
			}
		} );
	}

	void CollisionNodeManager::TickStreaming( job::Counter& sectorsPreload, Bool performQueries )
    {
        PC_SCOPE( Collision_TickStreaming );

		constexpr Uint32 c_maxActorsPerFrame = 150;
		constexpr Uint32 c_maxActorsPerTick = 64;
		constexpr Uint32 c_maxConcurrentInits[ c_maxLOD ] = { UINT32_MAX, 64, 16, UINT32_MAX };
		constexpr Uint32 c_maxActorOperationsPerTick[ c_maxLOD ] = { c_maxActorsPerTick, c_maxActorsPerTick, c_maxActorsPerTick, UINT32_MAX };
		constexpr Uint32 c_maxActorOperationsPerFrame[ c_maxLOD ] = { c_maxActorsPerFrame, c_maxActorsPerFrame, c_maxActorsPerFrame, UINT32_MAX };

		const job::Priority c_builderPriority[ c_maxLOD ] = { job::Priority::Immediate, job::Priority::CriticalPath, job::Priority::CriticalPath, job::Priority::CriticalPath };
        
        const Vector3 referencePostition = physics::GetReferencePosition( m_sceneId ).AsVector3();

		job::Counter sectorsCounter[ c_maxLOD ];
		sectorsCounter[0] += sectorsPreload;
		red::WeakPtr<world::CollisionNodeManager> weakThis = red::SharedFromPtr<world::CollisionNodeManager>( this );
		if( performQueries )
		{
			m_attachCounter = 0;
			m_detachedCounter = 0;
		}

		Uint32 attachCounterCurrentTick = 0;
		Uint32 detachedCounterCurrentTick = 0;
		CollisionIdSet toDetach{red::PoolDoubleBufferedFrame()};
        for( Uint32 lod = 0; lod < c_maxLOD; ++lod )
        {
			Uint32 currentInits = m_initInProgress[ lod ].GetValue();
			for( Uint32 i = 0; i < lod; ++i )
			{
				currentInits += m_initInProgress[ i ].GetValue();
			}

			if( ( c_maxActorOperationsPerTick[ lod ] <= m_detachedCounter ) && ( c_maxActorOperationsPerTick[ lod ] <= m_attachCounter )
				&& ( c_maxConcurrentInits[ lod ] <= currentInits ) )
			{
				// no room in this tick for this LOD, no point processing it
				continue;
			}
			
			job::Builder builder( c_builderPriority[ lod ] );
			builder.DispatchWait( sectorsCounter[lod] );

			RED_SCOPE_LOCK( m_nodeLock[ lod ] );
            const auto& bvh = m_nodesBVH[lod];
			auto& scratch = m_streamingQueryFrameResult[ lod ];
			if( performQueries )
			{
				const auto& nodeMap = m_nodesMap[ lod ];
				const auto& inRange = m_inRange[ lod ];
				auto& frameResults = m_streamingQueryFrameResultDistances[ lod ];
				frameResults.Clear();
				const Box areaBox = ComputeBoxForLOD( referencePostition, lod );
				scratch.Clear();
				bvh.Query( areaBox, [ &scratch, &frameResults, &inRange, &nodeMap, &referencePostition ]( const CollisionNodeId& id )
				{
					if( !inRange.Exist(id) )
					{
						if( const CollisionNodeEntry* entry = nodeMap.FindPtr( id ) )
						{
							Float distance =  entry->m_worldBounds.SquaredDistance( referencePostition );
							frameResults.PushBack( { id, distance } );
						}
					}
					scratch.Insert( id );
					return true;
				} );

				if( lod == 0 )
				{
					Bool cleanUpRequired = false;
					for( const physics::ProxyID& observerId : m_additionalObservers )
					{
						Bool isAlive = physics::IsAlive( observerId );
						if( isAlive )
						{
							Box observerBounds = physics::GetCompoundBounds( observerId );
							if( !areaBox.Contains( observerBounds ) )
							{
								bvh.Query( observerBounds, [ &scratch, &frameResults, &inRange, &nodeMap, &referencePostition ]( const CollisionNodeId& id )
								{
									if( !inRange.Exist( id ) )
									{
										if( const CollisionNodeEntry* entry = nodeMap.FindPtr( id ) )
										{
											Float distance = entry->m_worldBounds.SquaredDistance( referencePostition );
											frameResults.PushBack( { id, distance } );
										}
									}
									scratch.Insert( id );
									return true;
								} );
							}
						}
						cleanUpRequired |= !isAlive;
					}

					if( cleanUpRequired )
					{
						red::alg::EraseIf( m_additionalObservers, []( const physics::ProxyID& proxyId )
						{
							return !physics::IsAlive( proxyId );
						} );
					}
				}

				frameResults.StableSort();
			}

			toDetach.Clear();
			toDetach.Reserve( m_inRangeInitialized[ lod ].Size() );
			toDetach.Union( m_inRangeInitialized[lod] ); // m_inRange is contained in m_inRangeInitialized 
			toDetach.Difference( scratch );

#ifndef RED_CONFIGURATION_FINAL
			red::HashSet<CollisionNodeId> toDetachCopyForCancels = m_inRangeInitializing[ lod ];
			toDetachCopyForCancels.Difference( scratch );
			m_numCanceledInits += toDetachCopyForCancels.Size();
#endif
            for( CollisionNodeId id : toDetach )
            {
                if( CollisionNodeEntry* entry = m_nodesMap[ lod ].FindPtr( id ) )
                {
					if( !entry->IsFlagSet( ImmediateAttach ) )
					{
						if( entry->m_state == Initialized )
						{
							if( physics::IsAlive( entry->m_proxyId ) )
							{
								physics::DestroyProxy( entry->m_proxyId );
							}
							entry->m_state = NodeAttached;
							m_inRangeInitialized[ lod ].Remove( id );
						}
						else if( entry->m_state == Attached )
						{
							if( c_maxActorOperationsPerFrame[ lod ] > m_detachedCounter )
							{
								if( c_maxActorOperationsPerTick[ lod ] > detachedCounterCurrentTick )
								{
									physics::DetachProxy( m_sceneId, entry->m_proxyId );
									entry->m_state = Initialized;
									// x2 so we have half of attach limit for detaches - they aren't as time critical
									auto numObjects = physics::GetNumSubObjects( entry->m_proxyId ) * 2;
									detachedCounterCurrentTick += numObjects;
									m_detachedCounter += numObjects;
									m_inRange[ lod ].Remove( id );
								}
							}
						}
					}
                }
            }

			auto& toAttach = toDetach; // becomes toAttach now, reusing memory
			toAttach.Clear(); 
			toAttach.Reserve( scratch.Size() );
			toAttach.Union( scratch );
			toAttach.Difference( m_inRange[lod] );

			for( const CollisionQueryResult& result : m_streamingQueryFrameResultDistances[ lod ] )
			{
				const CollisionNodeId& id = result.m_nodeId;
				// on non query frame, we need an extra check as toAttach might have changed
				if( !performQueries ) 
				{
					if( toAttach.Empty() )
						break;

					if( toAttach.Exist( id ) )
					{
						toAttach.Remove( id );
					}
					else
					{
						continue;
					}
				}
                if( CollisionNodeEntry* entry = m_nodesMap[ lod ].FindPtr( id ) )
                {
					if( (entry->m_flags & ImmediateAttach) == 0 )
					{
						if( entry->m_state == NodeAttached && !entry->IsFlagSet(Initializing) )
						{
							if( c_maxConcurrentInits[ lod ] > currentInits )
							{
								if( entry->m_sectorId != c_noSectorId )
								{
									if( !m_sectorsInRange.Exist( entry->m_sectorId ) )
									{										
										// sector still not in range or waiting to load
										continue;
									}
								}
								if( cpweb_citycol::InitGateClosed() ) // CP2077 web [citycol] r3: always-loaded geometry not complete yet
								{
									continue;
								}

								entry->SetFlag( Initializing, true );
								m_initInProgress[ lod ].Increment();
								DispatchInitJob( builder, entry, weakThis, lod );								
								++currentInits;
#ifndef RED_CONFIGURATION_FINAL
								m_inRangeInitializing[ lod ].Insert( id );
								Uint32 currenctConcurrent = m_initInProgress[ lod ].GetValue();
								m_maxConcurrentProxies[ lod ] = math::Max( currenctConcurrent, m_maxConcurrentProxies[ lod ] );
#endif
							}
						}
						else if( entry->m_state == Initialized )
						{
							if( c_maxActorOperationsPerFrame[ lod ] > m_attachCounter )
							{
								auto numObjects = physics::GetNumSubObjects( entry->m_proxyId );
								if( c_maxActorOperationsPerTick[ lod ] >= (attachCounterCurrentTick + numObjects) )
								{
									physics::AttachProxy( m_sceneId, entry->m_proxyId );
									entry->m_state = Attaching;
									m_inRange[ lod ].Insert( id );
									attachCounterCurrentTick += numObjects;
									m_attachCounter += numObjects;
								}
							}
						}
					}
                }
            }

			if( (lod + 1) < c_maxLOD )
			{
				sectorsCounter[ ( lod + 1 ) ] += builder.ExtractWaitCounter();
			}
        }
    }

	Bool CollisionNodeManager::IsAreaAroundPlayerValid()
	{
		return m_isValid.GetValue();
	}

	void CollisionNodeManager::UpdateIsAreaAroundPlayerValid()
	{
		constexpr Uint32 lod = 0;
		RED_SCOPE_LOCK( m_nodeLock[ lod ] );

		const Vector3 referencePostition = physics::GetReferencePosition( m_sceneId ).AsVector3();
		const Vector3& ext = m_isMounted ? c_validPlayerExtMounted : c_validPlayerExtAfoot;
		const Box queryVolume = Box( referencePostition - ext, referencePostition + ext );
		Bool result = true;
		m_nodesBVH[0].Query( queryVolume, [ this, &result, lod ]( const CollisionNodeId& foundInstance )
		{
			RED_ASSERT( m_nodesMap[ lod ].KeyExist( foundInstance ) );
			auto& nodeEntry = m_nodesMap[ lod ][ foundInstance ];
			if( nodeEntry.m_state != Attached )
			{
				// node started initializing, but is not attached yet, the area is invalid
				result = false;
			}
			return true;
		} );

#ifndef RED_CONFIGURATION_FINAL
		m_isValidBVH.SetValue( result );
#endif
		if( !result )
		{
			static const CName staticGroup = RED_NAME_CONSTEXPR( "Static" );
			physics::QueryFilter queryFilter = physics::QueryFilter::ZERO();
			queryFilter.AddGroup( staticGroup );
			result = true;
			for( Uint32 i = 0; i < 4; ++i )
			{
				auto cornerBottom = queryVolume.CalcCorner( ( Box::ECorner )i );
				auto cornerTop = queryVolume.CalcCorner( ( Box::ECorner )(i+4) );

				physics::RaycastParams params = physics::RaycastParams::Create( cornerTop, cornerBottom, queryFilter );				
				params.SetFlags( physics::OverlapParams::STATIC_ONLY );
				physics::TraceResult outResult;
				auto hitResult = physics::SyncRaycast( m_sceneId, params, outResult );
				result &= hitResult == physics::SyncResult::Hit;
			}
		}
		result = cpweb_citycol::Gate( m_sceneId, referencePostition, result, m_isMounted ); // CP2077 web [citycol]
		m_isValid.SetValue( result );
	}

	void CollisionNodeManager::OnNodeCreated( const CollisionNodeId& id, const Box& bounds, Uint32 lod, CollisionInitFunc&& initFunc, Bool enableManagement, Uint64 sectorHash )
	{
        RED_FATAL_ASSERT( lod < c_maxLOD );
		RED_SCOPE_LOCK( m_nodeLock[lod] );

		if( !m_nodesMap[ lod ].KeyExist( id ) )
		{
            CollisionNodeEntry node;
            node.m_worldBounds = bounds;
            node.m_bvhId = m_nodesBVH[lod].Insert( bounds, id );
            node.m_lod = (Uint8)lod;
			node.m_sectorId = c_noSectorId;
			if( m_sectorHashToIndex.KeyExist( sectorHash ) )
			{
				node.m_sectorId = m_sectorHashToIndex[ sectorHash ];
			}
			node.SetFlag( ImmediateAttach, !enableManagement );
			node.m_initFunction = std::forward<CollisionInitFunc>(initFunc);
			m_nodesMap[ lod ].Insert( id, node );	
		}
	}

	void CollisionNodeManager::OnNodeDestroyed( const CollisionNodeId& id, Uint32 lod )
	{
		RED_SCOPE_LOCK( m_nodeLock[ lod ] );

		if( m_nodesMap[ lod ].KeyExist( id ) )
		{
			auto& node = m_nodesMap[ lod ][ id ];
			m_nodesBVH[node.m_lod].Remove( node.m_bvhId );
			m_nodesMap[ lod ].Remove( id );
#ifndef RED_CONFIGURATION_FINAL
			m_inRangeInitializing[ lod ].Remove( id );
#endif
			if( node.IsFlagSet( ImmediateAttach ) )
			{
				physics::DestroyProxy( node.m_proxyId );
				node.m_proxyId = physics::ProxyID();
			}
		}
	}

	void CollisionNodeManager::OnNodeInitialized( const CollisionNodeId& id, physics::ProxyID proxyId, Uint32 lod )
	{
		RED_SCOPE_LOCK( m_nodeLock[ lod ] );

		if( m_nodesMap[ lod ].KeyExist( id ) )
		{
			auto& node = m_nodesMap[ lod ][ id ];
			if( !node.IsFlagSet( ImmediateAttach ) )
			{
				if( node.IsFlagSet( Initializing ) && (node.m_state == NodeAttached ) )
				{
					node.m_state = Initialized;
					node.m_proxyId = proxyId;
					m_inRangeInitialized[ lod ].Insert( id );
				}
				else if( physics::IsAlive( proxyId ) ) // node detached while it was initializing
				{
					physics::DestroyProxy( proxyId );
				}
			}
			else
			{
				if( node.m_state == Created ) // Node finished initializing before attached
				{
					node.m_state = Initialized;
					node.m_proxyId = proxyId;
				}
				else if( node.m_state == NodeAttached ) // Node attached before initializing fully
				{
					node.m_proxyId = proxyId;
					physics::AttachProxy( m_sceneId, node.m_proxyId );
					node.m_state = Attaching;
					m_inRange[ lod ].Insert( id );
				}
			}

			node.SetFlag( Initializing, false );
#ifndef RED_CONFIGURATION_FINAL
			m_inRangeInitializing[ lod ].Remove( id );
#endif
		}
		else if( physics::IsAlive( proxyId ) ) // node destroyed before initalizing job finished
		{
			physics::DestroyProxy( proxyId );
		}
	}

	void CollisionNodeManager::OnNodeAttached( const CollisionNodeId& id, Uint32 lod )
	{
		RED_SCOPE_LOCK( m_nodeLock[ lod ] );

		if( m_nodesMap[ lod ].KeyExist( id ) )
		{
			auto& node = m_nodesMap[ lod ][ id ];
			if( !node.IsFlagSet( ImmediateAttach ) )
			{
				RED_ASSERT( node.m_state == Created );
				node.m_state = NodeAttached;
			}
			else
			{
				if( node.m_state == Initialized )
				{
					physics::AttachProxy( m_sceneId, node.m_proxyId );
					node.m_state = Attaching;
					m_inRange[ lod ].Insert( id );
				}
				else
				{
					node.m_state = NodeAttached;
				}
			}
		}
	}

	void CollisionNodeManager::OnNodeDetached( const CollisionNodeId& id, Uint32 lod )
	{
		RED_SCOPE_LOCK( m_nodeLock[ lod ] );

		if( m_nodesMap[ lod ].KeyExist( id ) )
		{
			auto& node = m_nodesMap[ lod ][ id ];
			Bool proxyAlive = physics::IsAlive( node.m_proxyId );
			Bool isManaged = !node.IsFlagSet( ImmediateAttach );
			if( isManaged )
			{
				node.m_state = Created;
			}
			else
			{
				node.m_state = proxyAlive ? Initialized : Created;
			}
			if( proxyAlive )
			{
				physics::DetachProxy( m_sceneId, node.m_proxyId );
				if( isManaged )
				{
					physics::DestroyProxy( node.m_proxyId );
					node.m_proxyId = physics::ProxyID();
				}
			}

			if( m_inRange[ node.m_lod ].Exist( id ) )
			{
				m_inRange[ node.m_lod ].Remove( id );
				m_inRangeInitialized[ node.m_lod ].Remove( id );
			}
			else if( m_inRangeInitialized[ node.m_lod ].Exist( id ) )
			{
				m_inRangeInitialized[ node.m_lod ].Remove( id );
			}
		}
	}

	void CollisionNodeManager::OnNodeDisplaced( const CollisionNodeId& id, Uint32 lod )
	{
		RED_SCOPE_LOCK( m_nodeLock[lod] );

		if( m_nodesMap[ lod ].KeyExist( id ) )
		{
			auto& node = m_nodesMap[ lod ][ id ];
			if( node.m_state == Attached ) // Proxy still within distance and node instance attached, usual case
			{
				node.m_state = Displaced;
			}
			else if( node.m_state == Initialized ) // we detached the node in TickSectors before the Pull was called
			{
				physics::AttachProxy( m_sceneId, node.m_proxyId );
				node.m_state = Displaced;
				m_inRange[ lod ].Insert( id );
			}
			// else - either already displaced or node instance was detached (OnDetach called) before pull
			// In both cases, do nothing
		}
	}

	void CollisionNodeManager::OnCacheLoaded( const THandle<physics::GeometryCache>& geometryCache )
	{
		RED_SCOPE_LOCK( m_cacheLock );
		m_geometryCache = geometryCache;
		for( auto index : geometryCache->m_sectorEntries.Indices() )
		{
			auto& sector = geometryCache->m_sectorEntries[ index ];
			if( sector.m_entrySize > 0 )
			{
				m_sectorsBVH.Insert( sector.m_sectorBounds, index );
			}
			m_sectorHashToIndex.Insert( sector.m_sectorHash, index );
		}
	}

	void CollisionNodeManager::OnPostWorldDetached()
	{
		RED_SCOPE_LOCK( m_cacheLock );
		for( SectorID id : m_sectorsInRange )
		{
			auto& sector = m_geometryCache->m_sectorEntries[ id ];
			if( sector.m_entrySize > 0 )
			{
				m_geometryCache->FreeSectorData( id );
			}
		}
		for( SectorID id : m_sectorsLoading )
		{
			auto& sector = m_geometryCache->m_sectorEntries[ id ];
			if( sector.m_entrySize > 0 )
			{
				m_geometryCache->FreeSectorData( id );
			}
		}
		m_sectorsInRange.Clear();
		m_sectorsLoading.Clear();
		m_sectorsBVH.Clear();
		m_geometryCache = nullptr;
	}

	void CollisionNodeManager::AddCollisionObserver( const physics::ProxyID& id )
	{
		RED_SCOPE_LOCK( m_nodeLock[0] );
		red::alg::PushBackUnique( m_additionalObservers, id );
	}

	void CollisionNodeManager::RemoveCollisionObserver( const physics::ProxyID& id )
	{
		RED_SCOPE_LOCK( m_nodeLock[ 0 ] );
		m_additionalObservers.Remove( id );
	}

	void CollisionNodeManager::SetIsMounted( Bool isMounted )
	{
		m_isMounted = isMounted;
	}

	static rend::DebugFilter DF_CollisionNodeBVH( "Physics/CollsisionNodeManager/BVH" );
	static rend::DebugFilter DF_CollisionNodeArea( "Physics/CollsisionNodeManager/InvalidatedArea" );
	static rend::DebugFilter DF_CollisionNodeStats( "Physics/CollsisionNodeManager/Stats" );
	static rend::DebugFilter DF_CollisionNodeNodeBounds( "Physics/CollsisionNodeManager/Nodes" );

	void CollisionNodeManager::OnRenderDebug( const RenderDebugContext& context, const THandle<physics::GeometryCache>& cache )
	{
#ifdef RED_ENABLE_RENDER_DEBUG
		rend::FragmentBuilder fb( context.debugDrawer );
        
        if( m_drawLODAreaMask || m_drawLODBoundsMask )
        {
            const Vector3 referencePostition = physics::GetReferencePosition( m_sceneId ).AsVector3();
            Color color[4] =
            {
                Color::RED(),
                Color::ORANGE(),
                Color::BLUE(),
                Color::GREEN(),
            };

            for( Uint32 i = 0; i < c_maxLOD; ++i )
            {
                if( m_drawLODAreaMask & ( 1 << i ) )
                {
                    const Box lodBox = ComputeBoxForLOD( referencePostition, i );
                    fb.SetColor( color[i] );
                    fb.AddWireBox( lodBox );
                }

                if( m_drawLODBoundsMask & ( 1 << i ) )
                {
                    for( CollisionNodeId id : m_inRange[i] )
                    {
                        if( const CollisionNodeEntry* entry = m_nodesMap[i].FindPtr( id ) )
                        {
                            fb.AddWireBox( entry->m_worldBounds );
                        }
                    }
                }

            }
        }

		if( m_drawBVHForLOD >= 0 && m_drawBVHForLOD < c_maxLOD )
		{
			RED_SCOPE_LOCK( m_nodeLock[ m_drawBVHForLOD ] );

			fb.EnableTransparency( true );
			Float alpha = 0.25f;

			for( auto it = m_nodesMap[ m_drawBVHForLOD ].Begin(); it != m_nodesMap[ m_drawBVHForLOD ].End(); ++it )
			{
				const auto& nodeEntry = it.Value();
				Color color = Color::RED();
				Uint16 state = (Uint16)nodeEntry.m_state;
				if( state > (Uint16)Attaching )
				{
					color = Color::GREEN();
				}
				else if( state > (Uint16)NodeAttached )
				{
					color = Color::YELLOW();
				}

				fb.SetColor( color );
				if( context.debugDrawer.GetMask().Test( DF_CollisionNodeNodeBounds ) )
				{
					fb.AddWorldText( nodeEntry.m_worldBounds.CalcCenter(), 0, 0, String::Printf( "%d (%d)", nodeEntry.m_proxyId.m_index, state ) );
				}
				fb.SetColor( color, alpha );
				if( context.debugDrawer.GetMask().Test( DF_CollisionNodeBVH ) && state < (Uint16)Attaching )
				{
					fb.AddSolidBox( nodeEntry.m_worldBounds );
				}
			}
		}
		Bool isValid = m_isValid.GetValue();
		if( context.debugDrawer.GetMask().Test( DF_CollisionNodeArea ) )
		{
			Vector3 referencePostition = physics::GetReferencePosition( m_sceneId ).AsVector3();
			
			const Vector3& ext = m_isMounted ? c_validPlayerExtMounted : c_validPlayerExtAfoot;
			const Box queryVolume = Box( referencePostition - ext, referencePostition + ext );
			fb.EnableTransparency( true );
			fb.SetColor( !isValid ? Color::RED(128) : Color::GREEN(128) );
			fb.AddWireBox( queryVolume );
			fb.SetColor( !isValid ? Color::RED(128) : Color::GREEN(128) );
			fb.AddSolidBox( queryVolume );
		}

		if( context.debugDrawer.GetMask().Test( DF_CollisionNodeStats ) )
		{
			RenderImgui(cache);
		}
#endif
	}

	void CollisionNodeManager::RenderImgui( const THandle<physics::GeometryCache>& cache)
	{
#ifdef RED_USE_IMGUI
		if( ImGui::Begin( "CollisionNodeManager", nullptr ) )
		{
			ImGui::Text( "Number of nodes tracked (per LOD): %d (%d, %d, %d)", m_nodesMap[ 0 ].Size() + m_nodesMap[ 1 ].Size() + m_nodesMap[ 2 ].Size(), 
				m_nodesMap[ 0 ].Size(), m_nodesMap[ 1 ].Size(), m_nodesMap[ 2 ].Size() );

			ImGui::Text( "Area under player: " );
			ImGui::SameLine();
			if( m_isValid.GetValue() )
			{
				ImGui::TextColored( ToImVec4( Color::GREEN() ), "OK" );
			}
			else
			{
				ImGui::TextColored( ToImVec4( Color::RED() ), "INVALID" );
			}

			ImGui::SameLine();
			ImGui::TextColored( ToImVec4( m_isValidBVH.GetValue() ? Color::GREEN() : Color::RED() ), " BVH" );
            ImGui::Separator();
            ImGui::SliderInt( "Draw BVH for LOD", &m_drawBVHForLOD, -1, c_maxLOD - 1 );

            ImGui::Separator();
            ImGui::Columns( 5 );
            ImGui::Text( "Draw LOD area" ); ImGui::NextColumn();
            for( Uint32 i = 0; i < c_maxLOD; ++i )
            {
                AnsiChar name[32];
                red::SNPrintFSafe( name, 32, "area LOD%u", i );
                ImGui::CheckboxFlags( name, &m_drawLODAreaMask, 1 << i );
                ImGui::NextColumn();
            }
            ImGui::Separator();
            ImGui::Text( "Draw LOD nodes bounds" ); ImGui::NextColumn();
            for( Uint32 i = 0; i < c_maxLOD; ++i )
            {
                AnsiChar name[32];
                red::SNPrintFSafe( name, 32, "node LOD%u", i );
                ImGui::CheckboxFlags( name, &m_drawLODBoundsMask, 1 << i );
                ImGui::NextColumn();
            }
            
            ImGui::Columns();

			Uint32 totalUniqueBuffersLoaded = 0;
			if( cache )
			{
				Uint32 sectorStats[ 4 ] = { 0 };
				for( auto& entry : cache->m_sectorEntries )
				{
					if( entry.m_entrySize > 0 )
					{
						++sectorStats[ entry.m_state ];
					}
				}

				ImGui::Separator();
				ImGui::Text( "Geometry cache stats:" );
				ImGui::Text( "Num loaded geometries: %d", cache->m_loadedGeometry.Size() );
				ImGui::Text( "Num shared shapes: %d", cache->m_createdShapes.Size() );
				ImGui::Separator();
				ImGui::Text( "Sector stats:" ); 			
				ImGui::SameLine();
				if( ImGui::Button( "Reset stats##resetSectorStatsButton" ) )
				{
					m_totalSectorLoads = 0.f;
					m_maxSectorLoadingTime = 0.f;
					m_totalSectorLoadingTime = 0.f;
					m_maxSectorsTotalConcurentSize = 0;
					m_maxSectorsLoadingConcurrently = 0;
				}
				ImGui::Columns( 4 );
				String sectorState[ 4 ] = { "Cleared", "Pending", "Loaded", "Abandoned" };
				ImGui::Separator();

				for( Uint32 i = 0; i < 4; ++i )
				{
					ImGui::Text( "%hs:", sectorState[ i ].AsChar() );
					ImGui::NextColumn();
				}
				ImGui::Separator();
				for( Uint32 i = 0; i < 4; ++i )
				{
					ImGui::Text( "%d", sectorStats[ i ] );
					ImGui::NextColumn();
				}
				ImGui::Columns();

				ImGui::Text( "Average sector loading time: %.2fs", math::EqualsZero( m_totalSectorLoads ) ? 0.f : m_totalSectorLoadingTime / m_totalSectorLoads );
				ImGui::Text( "Maximum sector loading time: %.2fs", m_maxSectorLoadingTime );
				ImGui::Text( "Total sectors loaded: %.1f ", m_totalSectorLoads );
				ImGui::Text( "Maximum sector concurrent loading size: %llu KB", m_maxSectorsTotalConcurentSize / 1000 );
				ImGui::Text( "Maximum sectors concurrently loading: %lu", m_maxSectorsLoadingConcurrently );

				if( m_sectorsLoading.Size() )
				{
					Uint64 totalSize = 0;
					for( SectorID currentLoading : m_sectorsLoading )
					{
						Uint32 sectorSize = 0;
						const auto& sectorEntry = m_geometryCache->m_sectorEntries[ currentLoading ];
						auto sectorCacheEntries = red::MakeArraySpan( m_geometryCache->m_sectorCacheEntries ).SubSpan( sectorEntry.m_entryOffset, sectorEntry.m_entrySize );
						for( auto& sectorCacheEntry : sectorCacheEntries )
						{
							sectorSize += sectorCacheEntry.m_entrySize;
						}
						totalSize += sectorSize;
						ImGui::Text( "Currently loading sector [%lu] size: %llu KB (Time loading: %.2fs", currentLoading, sectorSize / 1000, m_sectorLoadingTimeMap[ currentLoading ] );
					}
					m_maxSectorsLoadingConcurrently = math::Max( m_maxSectorsLoadingConcurrently, m_sectorsLoading.Size() );
					m_maxSectorsTotalConcurentSize = math::Max( m_maxSectorsTotalConcurentSize, totalSize );
				}
				else
				{
					ImGui::Text( "No sector loading" );
				}
				ImGui::Text( "Total cancellable inits (approximation): %lu ", m_numCanceledInits );
			}
		}
		ImGui::Columns();
		ImGui::Separator();
		ImGui::Text( "Collision manager stats:" );
		ImGui::Separator();
		ImGui::Columns( 5 );
		ImGui::Text( "" ); ImGui::NextColumn();
		ImGui::Text( "LOD0" ); ImGui::NextColumn();
		ImGui::Text( "LOD1" ); ImGui::NextColumn();
		ImGui::Text( "LOD2" ); ImGui::NextColumn();
		ImGui::Text( "LOD3" ); ImGui::NextColumn();
		ImGui::Separator();
		ImGui::Text( "Initializing" ); ImGui::NextColumn();
		Uint32 nodesStats[ 4 ][ 6 ] = { { 0 } };

		for( Uint32 i = 0; i < c_maxLOD; ++i )
		{
			Uint32 currenctConcurrent = m_initInProgress[ i ].GetValue();
			ImGui::Text( "%lu (%lu) ", currenctConcurrent, m_maxConcurrentProxies[i] );
			ImGui::NextColumn();

			for( Uint32 lod = 0; lod < c_maxLOD; ++lod )
			{
				RED_SCOPE_LOCK( m_nodeLock[ lod ] );
				for( auto it = m_nodesMap[ lod ].Begin(); it != m_nodesMap[ lod ].End(); ++it )
				{
					auto& nodeEntry = it.Value();
					++nodesStats[ lod ][ nodeEntry.m_state ];
				}
			}
		}
		ImGui::Separator();
		String state[ 7 ] = { "Created", "Node Attached" ,"Proxy Initialized", "Proxy Attaching", "Proxy Attached", "Displaced", "Frame Results" };
		for( Uint32 i = 0; i < 6; ++i )
		{
			ImGui::Text( "%hs:", state[ i ].AsChar() ); 
			ImGui::NextColumn();
			for( Uint32 lod = 0; lod < c_maxLOD; ++lod )
			{
				ImGui::Text( "%d", nodesStats[ lod ][ i ] );
				ImGui::NextColumn();
			}
			ImGui::Separator();
		}

		ImGui::Text( "%hs:", state[ 6 ].AsChar() );
		ImGui::NextColumn();
		for( Uint32 lod = 0; lod < c_maxLOD; ++lod )
		{
			ImGui::Text( "%d", m_streamingQueryFrameResultDistances[ lod ].Size() );
			ImGui::NextColumn();
		}
		ImGui::Separator();
		ImGui::End();
#endif // RED_USE_IMGUI
	}

    //////////////////////////////////////////////////////////////////////////

    enum AsyncSimulation : Int32
    {
        Disabled = -1,
        DisableRequest = 0,
        Enabled = 1,
    };

	RuntimeSystemPhysics::RuntimeSystemPhysics()
		: m_collisionNodeManager(red::CreateSharedPtr<CollisionNodeManager>())
		, m_cameraSystem(nullptr)
	{
	}

	SystemID RuntimeSystemPhysics::GetSystemID() const
	{
		return SYSTEM_ID;
	}

	// CP2077 web [sandbox-floor]: safety-net ground for ?sandbox sessions (gyms\gym_plate / gym_empty).
	// The gym's real floor is an editor box with an inline box collider (worldCollisionNode in one streamed
	// exterior sector; box primitives never go into the geometry cache, so a header-only cache is normal).
	// This adds a 1200 x 1200 m kinematic box whose top sits 5 cm BELOW z=0, so it never fights the real
	// plate but catches the player / vehicles if that sector's collision is missing or streams out.
	// Created through the engine proxy path (SimpleCollider + "World Static" preset) so query/simulation
	// filters and user data are set like any other world collision. No effect without ?sandbox in the URL.
	namespace cpweb_sandbox_floor
	{
		static std::atomic< Int32 > s_isSandbox{ -1 };
		static std::atomic< Bool > s_armed{ false };
		static physics::ProxyID s_proxy;
		static physics::SceneID s_scene;
		static Bool s_created = false;

		static Bool IsSandbox()
		{
			Int32 v = s_isSandbox.load();
			if ( v < 0 )
			{
#ifdef __EMSCRIPTEN__
				v = MAIN_THREAD_EM_ASM_INT( {
					try { const v = new URLSearchParams( location.search ).get( 'sandbox' ); return ( v !== null && v !== '0' && v !== 'false' ) ? 1 : 0; } catch ( e ) { return 0; }
				} );
#else
				v = 0;
#endif
				s_isSandbox.store( v );
			}
			return v > 0;
		}

		static void Arm( const THandle< physics::GeometryCache >& cache )
		{
			if ( !IsSandbox() )
				return;
			Uint32 sectors = 0, dataSectors = 0;
			if ( cache )
			{
				for ( auto index : cache->m_sectorEntries.Indices() )
				{
					++sectors;
					if ( cache->m_sectorEntries[ index ].m_entrySize > 0 )
						++dataSectors;
				}
			}
			std::fprintf( stderr, "[sandbox-floor] world loaded: geometry cache %s, %u sectors (%u with mesh/convex data); floor armed\n",
				cache ? "present" : "MISSING", sectors, dataSectors );
			s_armed.store( true );
		}

		// Called on the tick thread before the physics step (never while PhysX is simulating).
		static void Tick( physics::SceneID sceneId )
		{
			if ( !s_armed.load() || s_created || sceneId == physics::SceneID() )
				return;
			s_armed.store( false );

			physics::SimpleColliderDesc desc;
			desc.m_sourceType = physics::PhysicalSystemOwner::Unknown;
			desc.m_localToWorld = Transform( Vector4( 0.0f, 0.0f, -0.55f, 1.0f ), Quaternion::IDENTITY() );
			desc.m_colliders.PushBack( CreateHandle< physics::ColliderBox >( Vector3( 600.0f, 600.0f, 0.5f ) ) );
			desc.m_filterData.ApplyPreset( RED_NAME_CONSTEXPR( "World Static" ) );
			s_proxy = physics::CreateProxy( desc );
			if ( !physics::IsAlive( s_proxy ) )
			{
				std::fprintf( stderr, "[sandbox-floor] FAIL CreateProxy returned a dead proxy; no floor\n" );
				return;
			}
			physics::AttachProxy( sceneId, s_proxy );
			s_scene = sceneId;
			s_created = true;
			std::fprintf( stderr, "[sandbox-floor] floor attached: kinematic box 1200x1200x1 m, top z=-0.05, preset World Static\n" );
		}

		static void Release( physics::SceneID sceneId )
		{
			s_armed.store( false );
			if ( !s_created )
				return;
			if ( physics::IsAlive( s_proxy ) )
			{
				physics::DetachProxy( s_scene, s_proxy );
				physics::DestroyProxy( s_proxy );
			}
			s_proxy = physics::ProxyID();
			s_scene = physics::SceneID();
			s_created = false;
			std::fprintf( stderr, "[sandbox-floor] floor released (world detached)\n" );
		}
	}

	void RuntimeSystemPhysics::OnInitialize( RuntimeScene& scene, const RuntimeSystemInitInfo& initInfo, job::Counter& counter )
	{
		m_isPreview = ( initInfo.mode == RuntimeMode::Preview || initInfo.mode == RuntimeMode::GamePreview || initInfo.mode == RuntimeMode::Thumbnail );
		m_environmentSystem = scene.GetSystem< world::RuntimeSystemEnvironment >();
	}

	void RuntimeSystemPhysics::OnStreamingWorldLoaded( RuntimeScene& scene, job::Counter& counter )
	{
		auto streamingSystem = scene.GetSystem< world::RuntimeSystemWorldStreaming >();
		RED_FATAL_ASSERT( streamingSystem );

		// CP2077 web: asserts are compiled out in FINAL; a session whose .streamingworld failed to load has
		// a null streaming world (tolerated by RuntimeSystemWorldStreaming). Copying a handle out of a
		// null world would run atomics on garbage low memory (wasm "unaligned accesses" trap).
		if ( !streamingSystem )
		{
			return;
		}

		auto streamingWorld = streamingSystem->GetStreamingWorld();
		RED_FATAL_ASSERT( streamingWorld );
		if ( !streamingWorld )
		{
			std::fprintf( stderr, "CP2077 physics guard: no streaming world loaded; no geometry cache\n" );
			return;
		}

		m_geometryCache = Cast<physics::GeometryCache>(streamingWorld->m_geometryCacheResource.Get());
		RED_FATAL_ASSERT( m_geometryCache, "Geometry Cache is missing. Build and Play is required" );
		if ( !m_geometryCache )
		{
			std::fprintf( stderr, "CP2077 physics guard: streaming world has no geometry cache\n" );
			cpweb_sandbox_floor::Arm( nullptr );
			return;
		}
		job::Builder builder;
		m_geometryCache->LoadAlwaysLoadedSector( builder );
		counter += builder.ExtractWaitCounter();

		m_collisionNodeManager->OnCacheLoaded( m_geometryCache );
		cpweb_sandbox_floor::Arm( m_geometryCache );
		// CP2077 web [citycol]: hold on-foot locomotion until ground is under the player after this world load
		cpweb_citycol::ReadSwitches();
		cpweb_citycol::AlWorldLoaded( m_geometryCache.Get() );
		cpweb_citycol::s_lastRefValid = false;
		cpweb_citycol::Arm( "world loaded", Vector3::ZEROS() );
	}

	void RuntimeSystemPhysics::OnSetup( RuntimeScene& scene, const RuntimeSceneSetupInfo& setupInfo, job::Counter& counter )
	{
		m_sceneId = physics::CreateScene();
		m_collisionNodeManager->m_sceneId = m_sceneId;
	}

	void RuntimeSystemPhysics::OnPostWorldDetached( RuntimeScene& scene )
	{
		physics::BlockingSyncWaitSimulation( m_sceneId );
		cpweb_sandbox_floor::Release( m_sceneId );
		m_collisionNodeManager->OnPostWorldDetached();
		if( m_geometryCache )
		{
			m_geometryCache->FreeAlwaysLoadedSector();
			m_geometryCache = nullptr;	
		}
		{
			if( m_ragdollContactCallbackId != UINT32_MAX )
			{
				physics::UnregisterContactCallback( m_sceneId, m_ragdollContactCallbackId );
				m_ragdollContactCallbackId = UINT32_MAX;
			}

            physics::DestroyScene( m_sceneId );
		}

		m_sceneId = physics::SceneID();
	}

	void RuntimeSystemPhysics::OnGameInstanceAttached( game::IGameInstance& gameInstance )
	{
		m_cameraSystem = gameInstance.GetSystemPtr< game::ICameraSystem >();
	}

	void RuntimeSystemPhysics::OnGameInstanceDetached( game::IGameInstance& gameInstance )
	{
		cpweb_sandbox_floor::s_armed.store( false );
		TickWhileLoading( false );
		m_collisionNodeManager->m_isValid.SetValue( false );
	}

	void RuntimeSystemPhysics::TickWhileLoading( Bool )
	{
		cpweb_sandbox_floor::Tick( m_sceneId );
		cpweb_citycol::s_inLoadingTick.store( 1 ); // CP2077 web [citycol]
		m_collisionNodeManager->Tick();
		cpweb_citycol::s_inLoadingTick.store( 0 );
		physics::TickWhileLoading(m_sceneId);
	}

    void RuntimeSystemPhysics::OnUninitialize( RuntimeScene& scene )
	{
		if ( m_sceneId != physics::SceneID() )
		{
			physics::BlockingSyncWaitSimulation( m_sceneId );
			{
				if( m_ragdollContactCallbackId != UINT32_MAX )
				{
					physics::UnregisterContactCallback( m_sceneId, m_ragdollContactCallbackId );
					m_ragdollContactCallbackId = UINT32_MAX;
				}

				physics::DestroyScene( m_sceneId );
			}

			m_sceneId = physics::SceneID();
		}
	}

	void RuntimeSystemPhysics::OnRenderDebug( const RenderDebugContext& context )
	{
#ifdef RED_ENABLE_RENDER_DEBUG
		if ( !physics::IsAlive(m_sceneId) )
			return;

		if ( physics::QueriesDebug* queriesDebug = physics::GetQueriesDebug( m_sceneId ) )
			queriesDebug->OnRenderDebug( context.debugDrawer );

        m_editorCameraPosition = context.frameInfo.m_cameraReferencePosition;
        m_editorFOV = context.m_fov;

		{
			rend::FragmentBuilder builder( context.debugDrawer );

			// display error text if the proxy ID allocator is full
			if ( physics::IsProxyIdAllocatorFull() )
			{
				builder.SetColor( Color::LIGHT_RED() );
				builder.AddFlatText( 20, 50, "Too many physical proxies created", 2.0f );
			}

			if ( m_isPreview && physics::Config::cvEnableParticlesWorldPreview.Get() )
			{
				const Int32 height = builder.GetScreenHeight() - 20;
				builder.SetColor( Color::LIGHT_RED() );
				red::StringBuilder<String> stringBuilder;
				stringBuilder.Appendf( "%s.%s is enabled. It might affect the editor's performance.", physics::Config::cvEnableParticlesWorldPreview.GetGroup(), physics::Config::cvEnableParticlesWorldPreview.GetName() );
				builder.AddFlatText( 20, height, stringBuilder.AsChar() );
			}
		}

		m_collisionNodeManager->OnRenderDebug( context, m_geometryCache );
		RenderOverflowWarnings( context.debugDrawer );
#endif
	}

#ifndef RED_CONFIGURATION_FINAL	
	void RuntimeSystemPhysics::RenderOverflowWarnings( rend::IDebugDrawer& dd )
	{
		if( !Config::cvHidePhysicsOverflowWarnings.Get() )
		{
			red::memory::PoolHandle parentHandle = physics::PoolPhysics::GetHandle();

			auto checkPool = [ this ]( const red::memory::PoolHandle handle, String highDesc, String overflowDesc ) {
				RuntimeSystemPhysics::PoolEntry entry;
				red::memory::RuntimePoolMetrics metrics;
				red::memory::GetRuntimePoolMetrics( handle, metrics );
				const red::memory::u64 budget = red::memory::GetPoolBudget( handle );
				Float fill = (Float)metrics.bytesAllocated / (Float)budget;
				Float usage = fill * 100.f;
				if( (Uint64)metrics.bytesAllocated > budget )
				{
					entry.state = RuntimeSystemPhysics::OVERFLOWING;
					entry.description = String::Printf( overflowDesc.AsChar(), usage );
				}
				else
				{
					if( fill >= Config::cvPhysicsHighUsageThreshold.Get() )
					{
						entry.state = RuntimeSystemPhysics::HIGH;
						entry.description = String::Printf( highDesc.AsChar(), usage );
					}
				}

				entry.handle = handle;
				if( entry.state == RuntimeSystemPhysics::NONE )
				{
					m_poolEntries.Remove( entry );
				}
				else
				{
					Int32 index = m_poolEntries.GetIndex( entry );
					if( index >= 0 )
					{
						m_poolEntries[index] = entry;
					}
					else
					{
						m_poolEntries.PushBack( entry );
					}
				}
			};

			checkPool( physics::PoolPhysicsDestruction::GetHandle(), "Destruction memory budget at %.2f%%. Be mindful about placing new assets in the vicinity.", 
				"Destruction memory budget exceeded (%.2f%%)! The asset placement in the area needs to be revised!" );
			checkPool( physics::PoolPhysicsCollision::GetHandle(), "Static collision memory budget at %.2f%%.", "Static collision memory budget exceeded (%.2f%%)!" );
			checkPool( physics::PoolPhysicsQueries::GetHandle(), "Physics queries memory budget at %.2f%%.", "Too many physics scene queries - memory budget exceeded (%.2f%%)!" );
			checkPool( physics::PoolPhysicsRagdoll::GetHandle(), "Ragdoll memory budget at %.2f%%.", "Ragdoll memory budget exceeded (%.2f%%)!" );
			checkPool( physics::PoolPhysicsProxies::GetHandle(), "Proxies memory budget at %.2f%%.", "Too many physical proxies on scene - memory budget exceeded (%.2f%%)!" );

			if( m_poolEntries.Size() )
			{
				// add first entry at the end of the array (it's to make sure it doesn't popup suddenly when there is only one error, but rather slides into view seemlessly
				auto firstEntry = m_poolEntries[ 0 ];
				Uint32 size = m_poolEntries.Size();
				if( size > 1 )
				{
					red::ArraySpan<PoolEntry> subView = red::MakeArraySpan( &m_poolEntries[ 1 ], size - 1 );
					
					for( const Uint32 idx : subView.ReverseIndices() )
					{
						if( subView[idx] == firstEntry )
						{
							m_poolEntries.RemoveAt( idx + 1 );
							break;
						}
					}
					
					m_poolEntries.PushBack( firstEntry );
				}
				else
				{
					m_poolEntries.PushBack( firstEntry );
				}
						
				rend::FragmentBuilder builder( dd );

				rend::FontStyle fontStyle = {};
				fontStyle.size = 20;
				fontStyle.outlineWidth = 2;

				const Vector2 WarningWordSize = builder.MeasureText( "WARNING", fontStyle );

				const Int32 screenOffsetTop = (Int32)(WarningWordSize.Y * 2.0f); // to not overlap with rendering one
				const Int32 screenOffsetBot = (Int32)WarningWordSize.Y; // to not overlap with rendering one
				const Int32 topOffset = dd.GetHeight() - screenOffsetTop;
				const Int32 bottomOffset = dd.GetHeight() - screenOffsetBot;

				builder.SetPostImGui( true );
				builder.EnableTransparency( true );
				builder.SetColor( Color::BLACK( 192 ) );
				builder.AddFlatRect( 0.0f, (Float)topOffset, (Float)dd.GetWidth(), (Float)bottomOffset );

				Int32 x = (Int32)WarningWordSize.X + m_caret + 10;
				Bool removeFirst = false;
				Int32 firstOffset = 0;
				for( Uint32 i : m_poolEntries.Indices() )
				{
					const RuntimeSystemPhysics::PoolEntry& entry = m_poolEntries[ i ];
					String output = String::EMPTY();
					switch( entry.state )
					{
					case RuntimeSystemPhysics::PoolPreviousState::HIGH:
						builder.SetColor( Color::ORANGE() );
						builder.SetFontOutlineColor( Color::BLACK( 128 ) );
						break;
					case RuntimeSystemPhysics::PoolPreviousState::OVERFLOWING:
						builder.SetColor( Color::Lerp( 0.5f + 0.5f * sinf( EngineTime::GetNow() * 3.0f ), Color::RED(), Color::LIGHT_BLUE() ) );
						builder.SetFontOutlineColor( Color::BLACK( 128 ) );
						break;
					case RuntimeSystemPhysics::PoolPreviousState::NONE:
						continue;
					}
					output = entry.description;

					rend::TextLayout layout;
					layout.rect.m_left = x;
					layout.rect.m_right = x;
					layout.rect.m_top = topOffset;
					layout.rect.m_bottom = bottomOffset;
					layout.horizontalAlignment = rend::TextHorizontalAlignment::Left;
					layout.verticalAlignment = rend::TextVerticalAlignment::Center;
					layout.horizontalSpacing = 1;

					rend::FontStyle fontStyle = {};
					fontStyle.size = 20;
					fontStyle.outlineWidth = 2;
					builder.AddFlatTextLayout( layout, fontStyle, output );
					Int32 cutOff = ( (Int32)WarningWordSize.X - 10);
					if( i == 0 && x + (Int32)builder.GetLastTextWidth() < cutOff )
					{
						removeFirst = true;
						firstOffset = builder.GetLastTextWidth();
					}
					x += builder.GetLastTextWidth() + 64;
				}
				if( removeFirst )
				{
					m_poolEntries.RemoveAt( 0 );
					m_caret += firstOffset + 64;
				}
				m_caret -= Config::cvPhysicsOverflowWarningTextSpeed.Get();

				{
					builder.SetColor( Color( 168, 77, 62 ) );
					builder.AddFlatRect( 0.0f, (Float)topOffset, WarningWordSize.X, (Float)bottomOffset );

					rend::TextLayout layout;
					layout.rect.m_left = 0;
					layout.rect.m_right = (Int32)WarningWordSize.X;
					layout.rect.m_top = topOffset;
					layout.rect.m_bottom = bottomOffset;

					layout.horizontalAlignment = rend::TextHorizontalAlignment::Center;
					layout.verticalAlignment = rend::TextVerticalAlignment::Center;

					builder.SetColor( Color::WHITE() );
					builder.AddFlatTextLayout( layout, fontStyle, "WARNING" );
				}
			}
			else
			{
				m_caret = 0;
			}
		}
	}

#endif //RED_CONFIGURATION_FINAL

	Bool ShouldIgnoreTimeDilation( const tick::Info& tickInfo )
	{
		Bool isDilationActive = tickInfo.m_globalTimeDilationController.IsTimeDilationActive(RED_NAME_CONSTEXPR("SceneSystemFastForward"));
		isDilationActive |= tickInfo.m_globalTimeDilationController.IsTimeDilationActive(RED_NAME_CONSTEXPR("focusMode"));
		return isDilationActive;
	}

	Float GetTimeDelta(Float gameDt, const tick::Info& tickInfo )
	{
		return ShouldIgnoreTimeDilation( tickInfo ) ? tickInfo.m_realTimeDelta : gameDt;
	}
	void RuntimeSystemPhysics::RegisterUpdateFunctions( UpdatableSystemRegistrar& processor )
	{
#ifndef RED_CONFIGURATION_FINAL 
		processor.Register( UpdateTickGroup::FrameBegin, *this, "Physics/QueriesDebugNewFrame", 
			[this] ( const UpdateContext& updateContext, job::Builder& builder )
		{
			if ( auto* qd = physics::GetQueriesDebug( m_sceneId ) )
				qd->PrepareNewFrame();
		} );
#endif

		processor.RegisterInBucket( UpdateBucket::AllMask(), UpdateBucketPhase::PhysicsFlushBufferedState, *this, "Physics/UpdateInBucket",
			[ this ]( UpdateBucket::Enum bucket, const UpdateContext& updateContext, job::Builder& builder )
		{
			const Float timeDelta = GetTimeDelta( updateContext.m_timeDelta, updateContext.m_tickInfo );
			physics::UpdateInBucket( m_sceneId, bucket, timeDelta );

			job::Builder asyncBuilder;
			physics::ExecuteAsyncQueries( m_sceneId, asyncBuilder );
			m_asyncExecuteAsyncQueries += asyncBuilder.ExtractWaitCounter();
		});

		processor.RegisterInBucket( UpdateBucket::AllMask(), UpdateBucketPhase::PhysicsExecuteAsyncQueries, *this, "Physics/ExecuteAsyncQueries",
			[ this ]( UpdateBucket::Enum bucket, const UpdateContext& updateContext, job::Builder& builder )
		{
			builder.DispatchWait( m_asyncExecuteAsyncQueries );
		});

		processor.RegisterInBucket( UpdateBucket::AllMask(), UpdateBucketPhase::AnimationUpdate, *this, "CollisionNodeManager/TickKick",
			[ this ]( UpdateBucket::Enum bucket, const UpdateContext& updateContext, job::Builder& depBuilder )
		{
			job::Builder asyncBuilder;
			asyncBuilder.DispatchJob( "CollisionNodeManager/Tick", [ this, updateContext, bucket ]( const job::RunContext& depCtx )
			{
				m_collisionNodeManager->Tick( updateContext.m_timeDelta, ( bucket == UpdateBucket::Vehicle ), ( bucket == UpdateBucket::AttachedObject ) );
			} );
			m_asyncPostUpdateCounter += asyncBuilder.ExtractWaitCounter();
		} );

		processor.RegisterInBucket( UpdateBucket::AllMask(), UpdateBucketPhase::PostPhysicsTick, *this, "CollisionNodeManager/TickFinish",
			[ this ]( UpdateBucket::Enum bucket, const UpdateContext& updateContext, job::Builder& depBuilder )
		{
			depBuilder.DispatchWait( m_asyncPostUpdateCounter );
		} );

		processor.Register( UpdateTickGroup::PreBuckets, *this, "ApplyWind", [this]( const UpdateContext& updateContext, job::Builder& builder )
		{            
			const Float timeDelta = GetTimeDelta(updateContext.m_timeDelta, updateContext.m_tickInfo);
			cpweb_sandbox_floor::Tick( m_sceneId );
			cpweb_citycol::TickPre( m_sceneId ); // CP2077 web [citycol]: pending static scene-query rebuild
			physics::SimulateNestedScenes(builder, timeDelta);
			
            auto windVector = m_environmentSystem->GetWindVector();
			RED_SCOPE_SHARED_LOCK(m_clothMeshNodeLock);
			for(const auto& node : m_clothMeshNodes)
			{
				Vector4 lodData = Internal_CalcLodData();
				node->ProcessLOD(lodData.AsVector3(), lodData.W);
				node->ApplyWind(windVector);
			}
		} );

		// in editor finish simulation in the same frame. 
		processor.Register( UpdateTickGroup::PreRenderUpdate, *this, "Physics/KickAndWaitSimulation", [this]( const UpdateContext& updateContext, job::Builder& builder )
		{
			const Float timeDelta = GetTimeDelta(updateContext.m_timeDelta, updateContext.m_tickInfo);
			physics::KickSimulation( m_sceneId, timeDelta);
			physics::WaitSimulation( m_sceneId, builder );
		} );
	}

	physics::SyncResult RuntimeSystemPhysics::SyncRaycastN( const physics::RaycastParams& params, physics::TraceQueryBuffer& outResult ) const
	{
		return physics::SyncRaycastN( m_sceneId, params, outResult );
	}

	physics::SyncResult RuntimeSystemPhysics::SyncRaycast( const physics::RaycastParams& params, physics::TraceResult& outResult ) const
	{
		return physics::SyncRaycast( m_sceneId, params, outResult );
	}

	physics::SyncResult RuntimeSystemPhysics::SyncSweepN( const physics::SweepParams& params, physics::TraceQueryBuffer& outResult ) const
	{
		return physics::SyncSweepN( m_sceneId, params, outResult );
	}

	physics::SyncResult RuntimeSystemPhysics::SyncSweep( const physics::SweepParams& params, physics::TraceResult& outResult ) const
	{
		return physics::SyncSweep( m_sceneId, params, outResult );
	}

	physics::SyncResult RuntimeSystemPhysics::SyncOverlapN( const physics::OverlapParams& params, physics::TraceQueryBuffer& outResult ) const
	{
		return physics::SyncOverlapN( m_sceneId, params, outResult );
	}

	physics::SyncResult RuntimeSystemPhysics::SyncOverlap( const physics::OverlapParams& params, physics::TraceResult& outResult ) const
	{
		return physics::SyncOverlap( m_sceneId, params, outResult );
	}

	physics::AsyncQueryTokenPtr RuntimeSystemPhysics::AsyncRaycast( const physics::RaycastParams& params )
	{
		return physics::Raycast( m_sceneId, params );
	}

	physics::AsyncQueryTokenPtr RuntimeSystemPhysics::AsyncSweep( const physics::SweepParams& params )
	{
		return physics::Sweep( m_sceneId, params );
	}

	physics::AsyncQueryTokenPtr RuntimeSystemPhysics::AsyncOverlap( const physics::OverlapParams& params )
	{
		return physics::Overlap( m_sceneId, params );
	}

	physics::AsyncQueryTokenPtrN RuntimeSystemPhysics::AsyncRaycastN( const physics::RaycastParams& params )
	{
		return physics::RaycastN( m_sceneId, params );
	}

	physics::AsyncQueryTokenPtrN RuntimeSystemPhysics::AsyncSweepN( const physics::SweepParams& params )
	{
		return physics::SweepN( m_sceneId, params );
	}

	physics::AsyncQueryTokenPtrN RuntimeSystemPhysics::AsyncOverlapN( const physics::OverlapParams& params )
	{
		return physics::OverlapN( m_sceneId, params );
	}

	void RuntimeSystemPhysics::AsyncRaycast( const red::DynArray<physics::RaycastParams>& params, red::DynArray<physics::AsyncQueryTokenPtr>& outTokens )
	{
		if ( params.Empty() )
		{
			RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Physics ,"Called AsyncRaycast(array) with no params!" );
			return;
		}

		physics::AsyncRaycast( m_sceneId, params, outTokens );
	}

	void RuntimeSystemPhysics::AsyncSweep( const red::DynArray<physics::SweepParams>& params, red::DynArray<physics::AsyncQueryTokenPtr>& outTokens )
	{
		if ( params.Empty() )
		{
			RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Physics, "Called AsyncSweep(array) with no params!" );
			return;
		}

		physics::AsyncSweep( m_sceneId, params, outTokens );
	}

	void RuntimeSystemPhysics::AsyncOverlap( const red::DynArray<physics::OverlapParams>& params, red::DynArray<physics::AsyncQueryTokenPtr>& outTokens )
	{
		if ( params.Empty() )
		{
			RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Physics, "Called AsyncOverlap(array) with no params!" );
			return;
		}
		
		physics::AsyncOverlap( m_sceneId, params, outTokens );
	}

	void RuntimeSystemPhysics::SetRagdollCollisionCallback( const physics::ContactCallback& callback )
	{
		RED_SCOPE_LOCK( m_ragdollCallbackLock );
		if ( m_ragdollContactCallbackId == UINT32_MAX )
		{
			m_ragdollContactCallbackId = physics::RegisterContactCallback( m_sceneId, callback );
		}
	}

	void RuntimeSystemPhysics::RegisterClothMeshNode(const THandle<world::ClothMeshNodeInstance>& node)
	{
		RED_SCOPE_LOCK(m_clothMeshNodeLock);
		m_clothMeshNodes.PushBack(node);
	}

	void RuntimeSystemPhysics::UnregisterClothMeshNode(const THandle<world::ClothMeshNodeInstance>& node)
	{
		RED_SCOPE_LOCK(m_clothMeshNodeLock);
		red::alg::EraseIf(m_clothMeshNodes, [node](const auto& n){ return n == node; });
	}

	void RuntimeSystemPhysics::OnCollisionNodeCreated( const CollisionNodeId& id, const Box& bounds, Uint32 lod, CollisionInitFunc&& initFunc, Bool enableManagement, Uint64 sectorHash )
	{
		m_collisionNodeManager->OnNodeCreated( id, bounds, lod, std::forward<CollisionInitFunc>(initFunc), enableManagement, sectorHash );
	}

	void RuntimeSystemPhysics::OnCollisionNodeDestroyed( const CollisionNodeId& id, Uint32 lod )
	{
		m_collisionNodeManager->OnNodeDestroyed( id, lod );
	}

	void RuntimeSystemPhysics::OnCollisionNodeInitialized( const CollisionNodeId& id, physics::ProxyID proxyId, Uint32 lod )
	{
		m_collisionNodeManager->OnNodeInitialized( id, proxyId, lod );
	}

	void RuntimeSystemPhysics::OnCollisionNodeAttached( const CollisionNodeId& id, Uint32 lod )
	{
		m_collisionNodeManager->OnNodeAttached( id, lod );
	}

	void RuntimeSystemPhysics::OnCollisionNodeDetached( const CollisionNodeId& id, Uint32 lod )
	{
		m_collisionNodeManager->OnNodeDetached( id, lod );
	}

	void RuntimeSystemPhysics::OnCollisionNodeDisplaced( const CollisionNodeId& id, Uint32 lod )
	{
		m_collisionNodeManager->OnNodeDisplaced( id, lod );
	}

	void RuntimeSystemPhysics::AddCollisionObserver( const physics::ProxyID& id )
	{
		m_collisionNodeManager->AddCollisionObserver( id );
	}

	void RuntimeSystemPhysics::RemoveCollisionObserver( const physics::ProxyID& id )
	{
		m_collisionNodeManager->RemoveCollisionObserver( id );
	}

	void RuntimeSystemPhysics::SetIsMounted( Bool isMounted )
	{
		m_collisionNodeManager->SetIsMounted( isMounted );
	}

	Bool RuntimeSystemPhysics::IsAreaAroundPlayerValid()
	{
		return m_collisionNodeManager->IsAreaAroundPlayerValid();
	}

	Bool RuntimeSystemPhysics::IsInCriticalCollisionRange( const Vector3& position )
	{
		Box box = m_collisionNodeManager->ComputeBoxForLOD( physics::GetReferencePosition( m_sceneId ).AsVector3(), 0 );
		return box.ContainsExcludeEdges( position );
	}

	Float RuntimeSystemPhysics::GetPhysicsTimeDilation( const tick::Info& tickInfo ) const
	{
		return ShouldIgnoreTimeDilation( tickInfo ) ? 1.f : tickInfo.m_globalTimeDilation;
	}

	Float RuntimeSystemPhysics::GetNextConsumedDeltaTime( Float gameDt, const tick::Info& tickInfo ) const
	{
		return physics::GetNextConsumedDeltaTime( m_sceneId, GetTimeDelta( gameDt, tickInfo ) );
	}

	Vector4 RuntimeSystemPhysics::Internal_CalcLodData() const
	{
		Vector4 result = Vector4::ZERO_3D_POINT();

		if (!m_cameraSystem || !m_cameraSystem->GetActiveCamera() )
		{
			const Float renderFOV = m_editorFOV;
			result.X = m_editorCameraPosition.X;
			result.Y = m_editorCameraPosition.Y;
			result.Z = m_editorCameraPosition.Z;
			result.W = (renderFOV > 0.f) ? CalcFovDistanceMultiplier(renderFOV) : 1.f;
		}
		else
		{
			const auto& data = m_cameraSystem->GetActiveCameraData();

			const Float cameraFOV = data.m_fov;
			const Float inverseCameraZoom = 1.f / data.m_zoom;
			const Float renderFOV = cameraFOV > 0.f ? cameraFOV * inverseCameraZoom : cameraFOV;

			result.X = data.m_position.x.AsFloat();
			result.Y = data.m_position.y.AsFloat();
			result.Z = data.m_position.z.AsFloat();
			result.W = (renderFOV > 0.f) ? CalcFovDistanceMultiplier(renderFOV) : 1.f;
		}

		return result;
	}

	const physics::SceneID RuntimeSystemPhysics::GetSceneID() const
    {
        return m_sceneId;
    }

	physics::AsyncQueryTokenPtrN RuntimeSystemPhysics::QueryUnderwaterLevel(const Vector3& cameraReferencePosition)
	{
		const Float fftRange = 1.0f;
		const Vector3 waterTest = cameraReferencePosition - Vector3(0.0, 0.0, fftRange);

		const static physics::QueryFilter filter = physics::QueryFilter::Make(RED_NAME_CONSTEXPR("Water"));
		physics::RaycastParams params = physics::RaycastParams::Create(waterTest, Vector3(0.0f, 0.0f, 1.0), 10000.0f, filter)
			.SetUseCase(physics::QueryUseCase::GeomDescription)
			.SetFlags(physics::RaycastParams::MESH_BOTH_SIDES);

		return physics::RaycastN( m_sceneId, params );
	}

	void RuntimeSystemPhysics::FilterUnderwaterQueryParams(physics::AsyncQueryTokenPtrN physicsQueryToken, const THandle<CMesh> patchMesh,
		const Float patchDepthOffset, Float& waterDepth, Float& waterLevel, TRenderPtr<IRenderResource>& fftTexture, Float& waterSize, Vector4& waterChopiness, CName appearanceName )
	{
		const Float distanceToSurface = physicsQueryToken->m_results.Back().m_distance;
		const Float surfaceLevel = physicsQueryToken->m_results.Back().m_worldPosition.Z;
		if ( patchMesh )
		{
			THandle< mesh::MeshAppearance > appearance = patchMesh->GetAppearanceSafe( appearanceName );
			for ( Uint32 chunkIndex = 0; chunkIndex < appearance->GetNumChunks(); chunkIndex++ )
			{
				const THandle<IMaterial> material = appearance->GetChunkMaterialData( chunkIndex );
				material->ExtractTextureParam( RED_NAME_CONSTEXPR( "WaterFFT" ), fftTexture, true );
				material->ExtractScalarParam( RED_NAME_CONSTEXPR( "WaterSize" ), waterSize );
				material->ExtractVectorParam( RED_NAME_CONSTEXPR( "Choppiness" ), waterChopiness );
			}
		}

		if (patchDepthOffset > 0.0f)
		{
			waterDepth = distanceToSurface < patchDepthOffset ? distanceToSurface : -1.0f;
			waterLevel = distanceToSurface < patchDepthOffset ? surfaceLevel : -10000.0f;
		}
		else
		{
			waterDepth = distanceToSurface;
			waterLevel = surfaceLevel;
		}
	}

	physics::SceneID GetSceneID(const world::NodeInstanceInitializationContext& ctx)
	{
		if( ctx.scene )
		{
			return ctx.scene->GetSystem<world::RuntimeSystemPhysics>()->GetSceneID();
		}

		return physics::SceneID();
	}

	physics::SceneID GetSceneID(const RuntimeScene& scene)
	{
		return scene.GetSystem<world::RuntimeSystemPhysics>()->GetSceneID();
	}

	physics::SceneID GetSceneID(const ent::EntityAttachContext & context)
	{
		if( context.sceneService )
		{
			return context.sceneService->GetSystem<world::RuntimeSystemPhysics>()->GetSceneID();
		}

		return physics::SceneID();
	}

	physics::SceneID GetSceneID(const ent::ComponentInitializeContext& info)
	{
		if( info.entityOwner )
		{
			return info.entityOwner->GetSceneSystem<world::RuntimeSystemPhysics>()->GetSceneID();
		}

		return physics::SceneID();
	}

	physics::SceneID GetSceneID(const ent::ComponentAttachContext& info)
	{
		if( info.sceneService )
		{
			return info.sceneService->GetSystem<world::RuntimeSystemPhysics>()->GetSceneID();
		}

		return physics::SceneID();
	}

	void RegisterClothMeshNode(const RuntimeScene& scene, const THandle<world::ClothMeshNodeInstance>& node)
	{
		auto* phys = scene.GetSystem<world::RuntimeSystemPhysics>();
		phys->RegisterClothMeshNode(node);
	}

	void UnregisterClothMeshNode(const RuntimeScene& scene, const THandle<world::ClothMeshNodeInstance>& node)
	{
		auto* phys = scene.GetSystem<world::RuntimeSystemPhysics>();
		phys->UnregisterClothMeshNode(node);
	}

	void OnCollisionNodeCreated( const RuntimeScene* scene, const CollisionNodeId& id, const Box& bounds, Uint32 lod, CollisionInitFunc&& initFunc, Bool enableBVHAttachDetach, Uint64 sectorHash )
	{
		if( scene )
		{
			auto* physics_system = scene->GetSystem<world::RuntimeSystemPhysics>();
			physics_system->OnCollisionNodeCreated(id, bounds, lod, std::forward<CollisionInitFunc>( initFunc ), enableBVHAttachDetach, sectorHash );
		}
	}

	void OnCollisionNodeDestroyed(const RuntimeScene* scene, const CollisionNodeId& id, Uint32 lod )
	{
		auto* physics_system = scene->GetSystem<world::RuntimeSystemPhysics>();
		physics_system->OnCollisionNodeDestroyed(id, lod);
	}

	void OnCollisionNodeAttached( const RuntimeScene& scene, const CollisionNodeId& id, Uint32 lod )
	{
		auto* physics_system = scene.GetSystem<world::RuntimeSystemPhysics>();
		physics_system->OnCollisionNodeAttached(id, lod);
	}

	void OnCollisionNodeDetached( const RuntimeScene& scene, const CollisionNodeId& id, Uint32 lod )
	{
		auto* physics_system = scene.GetSystem<world::RuntimeSystemPhysics>();
		physics_system->OnCollisionNodeDetached(id, lod);
	}

	void OnCollisionNodeInitialized( const RuntimeScene* scene, const CollisionNodeId& id, physics::ProxyID proxyId, Uint32 lod )
	{
		auto* physics_system = scene->GetSystem<world::RuntimeSystemPhysics>();
		physics_system->OnCollisionNodeInitialized( id, proxyId, lod );
	}

	void OnCollisionNodeDisplaced( const RuntimeScene* scene, const CollisionNodeId& id, Uint32 lod )
	{
		auto* physics_system = scene->GetSystem<world::RuntimeSystemPhysics>();
		physics_system->OnCollisionNodeDisplaced( id, lod );
	}

	void AddCollisionObserver( const RuntimeScene* scene, const physics::ProxyID& id )
	{
		auto* physics_system = scene->GetSystem<world::RuntimeSystemPhysics>();
		physics_system->AddCollisionObserver( id );
	}

	void RemoveCollisionObserver( const RuntimeScene* scene, const physics::ProxyID& id )
	{
		auto* physics_system = scene->GetSystem<world::RuntimeSystemPhysics>();
		physics_system->RemoveCollisionObserver( id );
	}

#ifndef RED_CONFIGURATION_FINAL
	physics::PhysXGeometryWrapper::Cache& RuntimeSystemPhysics::GetGeometryWrapperCache() const
	{
		return physics::GetGeometryWrapperCache( m_sceneId );
	}
	
	res::ResourcePath ComputeGeometryCacheResourcePathFromWorldPath( res::ResourcePath path )
	{
		auto file_name = red::paths::ExtractFileStem(path.ToStringView());
		auto parent_path = red::paths::ExtractParentPath(path.ToStringView());
		auto path_str = red::String::Printf("%hssectors\\_generated\\collisions\\%hs.%hs", parent_path.AsChar(), file_name.AsChar(), physics::GeometryCache::GetFileExtension());
		return res::ResourcePath::Build( path_str );
	}

	res::ResourcePath ComputeGeometryCacheArtifactResourcePathFromWorldPath( res::ResourcePath path )
	{
		auto file_name = red::paths::ExtractFileStem( path.ToStringView() );
		auto parent_path = red::paths::ExtractParentPath( path.ToStringView() );
		auto path_str = red::String::Printf( "%hssectors\\_generated\\collisions\\%hs.%hs", parent_path.AsChar(), file_name.AsChar(), physics::GeometryCacheArtifact::GetFileExtension() );
		return res::ResourcePath::Build( path_str );
	}
#endif

}

#ifdef PHYSICS_DEBUG
#pragma optimize("", on)
#endif
