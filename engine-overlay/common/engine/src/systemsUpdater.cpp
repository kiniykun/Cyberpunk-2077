#include "build.h"

#include "../../../common/redReflection/include/resourceDepot.h"
#include "../../../common/redReflection/include/scriptableThreadSafetyMonitor.h"
#include "../../../common/redJobs2/include/jobRunner.h"
#include "../../../common/networkCore/include/netPeerId.h"

#include "updatableSystem.h"
#include "systemsUpdater.h"
#include <atomic> // [skperf] round 55 tick probe
#include <cstdio>
#include <cstring>
#include <mutex>
#include <emscripten/emscripten.h>
namespace cp2077 { namespace web { extern std::atomic<int> skperfEncodeProbe; } }
namespace cpTickProbe
{
	inline Bool On() { return ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 1 ) != 0; }
	enum Kind { K_Group, K_Bucket, K_Phase, K_Count };
	constexpr Uint32 kMax = 32;
	std::mutex s_lock;
	double s_begin[ K_Count ][ kMax ] = {}, s_sum[ K_Count ][ kMax ] = {}, s_frameBegin = 0, s_frameSum = 0, s_windowStart = 0;
	unsigned s_count[ K_Count ][ kMax ] = {}, s_frames = 0;
	const char* const c_groupNames[] = { "FrameBegin", "MP_UpdateSnapshots", "EntityUpdateState", "PreBuckets", "Buckets", "PostBuckets",
		"CameraUpdate", "PlayerAimUpdate", "PostPlayerAimUpdate", "MappinsUpdate", "BlackboardCallbacks2", "PreRenderUpdate", "MP_CaptureSnapshots" };
	const char* const c_phaseNames[] = { "EntitiesPreTick", "EntitiesServiceEvents", "PrePhysicsTick", "UpdateTransformPrePhysics",
		"PhysicsFlushBufferedState", "PhysicsExecuteAsyncQueries", "PostPhysicsSyncResults", "UpdateTransformPostPhysics", "AnimationUpdate",
		"PostPhysicsTick", "EntitiesPostTick", "EntitiesPostServiceEvents" };
	void Print( double now )
	{
		char line[ 3000 ]; std::size_t used = 0;
		auto add = [ & ]( const char* format, auto... args )
		{
			if ( used + 1 >= sizeof( line ) ) return;
			const int n = std::snprintf( line + used, sizeof( line ) - used, format, args... );
			if ( n > 0 ) used = ( used + std::size_t( n ) < sizeof( line ) ) ? used + std::size_t( n ) : sizeof( line ) - 1;
		};
		const double f = s_frames ? double( s_frames ) : 1.0;
		add( "CP2077 skperf tick probe: frames=%u updateWallMs=%.2f perFrame ms | groups:", s_frames, s_frameSum / f );
		for ( Uint32 i = 0; i < kMax; ++i ) if ( s_count[ K_Group ][ i ] ) add( " [%s %.2f]", i < sizeof( c_groupNames ) / sizeof( c_groupNames[ 0 ] ) ? c_groupNames[ i ] : "?", s_sum[ K_Group ][ i ] / f );
		add( " | buckets:" );
		for ( Uint32 i = 0; i < kMax; ++i ) if ( s_count[ K_Bucket ][ i ] ) add( " [b%u %.2f]", i, s_sum[ K_Bucket ][ i ] / f );
		add( " | phases (all buckets):" );
		for ( Uint32 i = 0; i < kMax; ++i ) if ( s_count[ K_Phase ][ i ] ) add( " [%s %.2f]", i < sizeof( c_phaseNames ) / sizeof( c_phaseNames[ 0 ] ) ? c_phaseNames[ i ] : "?", s_sum[ K_Phase ][ i ] / f );
		std::fprintf( stderr, "%s\n", line );
		for ( auto& row : s_sum ) for ( auto& v : row ) v = 0;
		for ( auto& row : s_count ) for ( auto& v : row ) v = 0;
		s_frames = 0; s_frameSum = 0; s_windowStart = now;
	}
	void Mark( const char* name, Uint32 index )
	{
		if ( index >= kMax || !name ) return;
		const Bool begin = name[ 0 ] == 'B';
		const Kind kind = std::strstr( name, "Group" ) ? K_Group : std::strstr( name, "Bucket" ) ? K_Bucket : K_Phase;
		const double now = emscripten_get_now();
		std::lock_guard< std::mutex > guard( s_lock );
		if ( begin )
		{
			s_begin[ kind ][ index ] = now;
			if ( kind == K_Group && index == 0 ) s_frameBegin = now;
			return;
		}
		if ( s_begin[ kind ][ index ] > 0 ) { s_sum[ kind ][ index ] += now - s_begin[ kind ][ index ]; ++s_count[ kind ][ index ]; }
		if ( kind == K_Group && index + 1 == Uint32( UpdateTickGroup::COUNT ) && s_frameBegin > 0 )
		{
			s_frameSum += now - s_frameBegin; ++s_frames;
			if ( s_windowStart == 0 ) s_windowStart = now;
			else if ( now - s_windowStart >= 2000.0 ) Print( now );
		}
	}
}
#include "../../redJobs2/include/jobBuilder.h"

#include "../../imgui/include/redImGui.h"
#include "../../imgui/src/imguiColumns.h"

namespace dd
{
	static red::CrashDataThreadLocal<String, 128> updateTickGroup("SystemUpdate", "UpdateTickGroup");
	static red::CrashDataThreadLocal<String, 128> updateDebugName("SystemUpdate", "DebugName");
}

namespace helper
{
	Debug_TickGroupInfo g_debugTickGroupInfo;

    static const char* GetSyncGroupName( UpdateTickGroup group )
    {
        switch( group )
        {
        case UpdateTickGroup::FrameBegin                       : return "FrameBegin";
        case UpdateTickGroup::Multiplayer_UpdateStateSnapshots : return "Multiplayer_UpdateStateSnapshots";
        case UpdateTickGroup::EntityUpdateState                : return "EntityUpdateState";
        case UpdateTickGroup::PreBuckets                       : return "PreBuckets";
		case UpdateTickGroup::Internal_Buckets                 : return "Buckets";
        case UpdateTickGroup::PostBuckets                      : return "PostBuckets";
        case UpdateTickGroup::CameraUpdate                     : return "CameraUpdate";
        case UpdateTickGroup::PlayerAimUpdate                  : return "PlayerAimUpdate";
        case UpdateTickGroup::PostPlayerAimUpdate              : return "PostPlayerAimUpdate";
        case UpdateTickGroup::MappinsUpdate                    : return "MappinsUpdate";
        case UpdateTickGroup::BlackboardCallbacks_SecondPass   : return "BlackboardCallbacks_SecondPass";
        case UpdateTickGroup::PreRenderUpdate                  : return "PreRenderUpdate";
        case UpdateTickGroup::Multiplayer_CaptureStateSnapshots: return "Multiplayer_CaptureStateSnapshots";
        }

        return "<Unknown>";
    }

    static const char* GetBucketPhaseName( UpdateBucketPhase phase )
    {
        switch( phase )
        {
        case UpdateBucketPhase::EntitiesPreTick           : return "Entities_PreTick";
        case UpdateBucketPhase::EntitiesServiceEvents     : return "Entities_ServiceEvents";
        case UpdateBucketPhase::PrePhysicsTick            : return "PrePhysicsTick";
        case UpdateBucketPhase::UpdateTransformPrePhysics : return "UpdateTransformPrePhysics";
        case UpdateBucketPhase::PhysicsFlushBufferedState : return "PhysicsFlushBufferedState";
        case UpdateBucketPhase::PostPhysicsSyncResults    : return "PostPhysicsSyncResults";
        case UpdateBucketPhase::UpdateTransformPostPhysics: return "UpdateTransformPostPhysics";
        case UpdateBucketPhase::AnimationUpdate           : return "AnimationUpdate";
        case UpdateBucketPhase::PostPhysicsTick           : return "PostPhysicsTick";
        case UpdateBucketPhase::EntitiesPostTick          : return "Entities_PostTick";
        case UpdateBucketPhase::EntitiesPostServiceEvents : return "Entities_PostServiceEvents";
        }

        return "<Unknown>";
    }

    static void CallUpdateFn( const SystemUpdateNodeSetup& setup, const UpdateContext& updateContext, job::Builder& builder )
    {
		RED_SET_SCOPED_CRASH_DATA(dd::updateTickGroup, GetSyncGroupName(setup.m_hook.group));
        RED_SET_SCOPED_CRASH_DATA(dd::updateDebugName, setup.m_info.debugName );

		g_debugTickGroupInfo.m_group = setup.m_hook.group;
		g_debugTickGroupInfo.m_bucketPhase = setup.m_hook.phase;
		
        setup.m_callback.group( updateContext, builder );
    }

    static void CallUpdateFn( UpdateBucket::Enum bucket, const SystemUpdateNodeSetup& setup, const UpdateContext& updateContext, job::Builder& builder )
    {
        RED_SET_SCOPED_CRASH_DATA( dd::updateTickGroup, GetSyncGroupName( setup.m_hook.group ) );
        RED_SET_SCOPED_CRASH_DATA( dd::updateDebugName, setup.m_info.debugName );

		g_debugTickGroupInfo.m_group = setup.m_hook.group;
		g_debugTickGroupInfo.m_bucketPhase = setup.m_hook.phase;

        setup.m_callback.bucket( bucket, updateContext, builder );
    }
}

const Debug_TickGroupInfo& Debug_GetCurrentUpdateTickGroup()
{
	return helper::g_debugTickGroupInfo;
}

class SystemsUpdater::Node
{
public:
	explicit Node( const SystemUpdateNodeSetup& setup )
		: m_setup( setup )
		, m_isActive( true )
		, m_instrumentationObjectPtr( red::CreateUniquePtr< red::InstrumentationObject >( setup.m_info.debugName, setup.m_info.profilerChannel ) )
	{}

    void SetActive( Bool value ) { m_isActive = value; }

    const rtti::ClassType* GetNodeClassType() const { return m_setup.m_info.classType; }
    const AnsiChar*        GetDebugName    () const { return m_setup.m_info.debugName; }
	Uint32				   GetProfilerColor() const { return m_instrumentationObjectPtr->m_profilerColor; }

    Uint32            GetBucketMask () const { return m_setup.m_hook.bucketMask; }
    UpdateBucketPhase GetBucketPhase() const { return m_setup.m_hook.phase; }

    Bool IsActive() const { return m_isActive; }
	Bool IsInBucket( UpdateBucket::Enum bucket ) const { return (RED_FLAG( bucket ) & m_setup.m_hook.bucketMask) != 0; }

	//! Add the job to be executed on this node
	void LinkJob_NoFence( job::Builder& builder, const UpdateContext& updateContext ) const
	{
		RED_FATAL_ASSERT( m_isActive );

		builder.DispatchJob< job::Fence::None >( *m_instrumentationObjectPtr, [this, updateContext]( const job::RunContext& runContext ) {
			job::Builder depBuilder{ runContext };
			ScopedProfilerChannel scopedChannel{ m_instrumentationObjectPtr->m_forceChannel == PBC_NONE ? PBC_RUNTIMESYSTEM : m_instrumentationObjectPtr->m_forceChannel };
			helper::CallUpdateFn( m_setup, updateContext, depBuilder );
		} );
	}

	void LinkBucketJob_NoFence( job::Builder& builder, UpdateBucket::Enum bucket, const UpdateContext& updateContext ) const
	{
		RED_FATAL_ASSERT( m_isActive );
		builder.DispatchJob< job::Fence::None >( *m_instrumentationObjectPtr, [this, bucket, updateContext]( const job::RunContext& runContext )
		{
			job::Builder depBuilder{ runContext };
			ScopedProfilerChannel scopedChannel{ m_instrumentationObjectPtr->m_forceChannel == PBC_NONE ? PBC_RUNTIMESYSTEM : m_instrumentationObjectPtr->m_forceChannel };

			helper::CallUpdateFn( bucket, m_setup, updateContext, depBuilder );
		} );
    }

private:
	SystemUpdateNodeSetup m_setup;
	red::UniquePtr< red::InstrumentationObject > m_instrumentationObjectPtr;
	Bool m_isActive;
};

class SystemsUpdater::Updater : red::NonCopyable
{
	RED_USE_MEMORY_POOL( red::PoolEngine );

public:
	typedef red::ArraySpan< const rtti::ClassType* const > TTickFilters;

	Updater()
		: m_updateNodes((Uint32)UpdateTickGroup::COUNT, red::PoolEngine() )
	{
	
		for ( Uint32 i = 0; i < (Uint32)UpdateTickGroup::COUNT; ++i )
		{
			m_groupInstrumentation.EmplaceBack( red::CreateUniquePtr<red::InstrumentationObject>( helper::GetSyncGroupName( (UpdateTickGroup)i ) ) );
		}
		
        for( Uint32 i = 0; i < (Uint32)UpdateBucketPhase::COUNT; ++i )
        {
            m_phaseInstrumentation.EmplaceBack( red::CreateUniquePtr<red::InstrumentationObject>( helper::GetBucketPhaseName( (UpdateBucketPhase)i ) ) );
        }

		for ( Uint32 i = 0; i < UpdateBucket::Count; ++i )
		{
			m_bucketInstrumentation.EmplaceBack( red::CreateUniquePtr<red::InstrumentationObject>( UpdateBucket::name[i] ) );
		}
	}

	void SetTickFilters( const TTickFilters& enabledClassTypes, const TTickFilters& exceptClassTypes = TTickFilters() )
	{
		auto findNodeClass = []( const rtti::ClassType* classTypeToFind, const red::ArraySpan< const rtti::ClassType* const >& classTypesToSearch ) -> Bool
		{
			const auto findIt = std::find_if( classTypesToSearch.begin(), classTypesToSearch.end(), [classTypeToFind]( const rtti::ClassType* searchedClassType )
			{
				return classTypeToFind->IsA( searchedClassType );
			} );

			return findIt != classTypesToSearch.end();
		};

		for ( Uint32 i = 0; i < (Uint32)UpdateTickGroup::COUNT; ++i )
		{
			for ( Node& node : m_updateNodes[ i ] )
			{
				const auto classType = node.GetNodeClassType();
				const Bool isActive = ( findNodeClass( classType, enabledClassTypes ) && !findNodeClass( classType, exceptClassTypes ) );
				node.SetActive( isActive );
			}
		}
	}

	void ModTickFilters( const TTickFilters& modifiedClassTypes, Bool enabled )
	{
		auto findNodeClass = []( const rtti::ClassType* classTypeToFind, const red::ArraySpan< const rtti::ClassType* const >& classTypesToSearch ) -> Bool
		{
			const auto findIt = std::find_if( classTypesToSearch.begin(), classTypesToSearch.end(), [ classTypeToFind ]( const rtti::ClassType* searchedClassType )
			{
				return classTypeToFind->IsA( searchedClassType );
			} );

			return findIt != classTypesToSearch.end();
		};

		for( Uint32 i = 0; i < ( Uint32 )UpdateTickGroup::COUNT; ++i )
		{
			for( Node& node : m_updateNodes[ i ] )
			{
				const auto classType = node.GetNodeClassType();
				if( const Bool toBeModified = findNodeClass( classType, modifiedClassTypes ) )
				{
					node.SetActive( enabled );
				}
			}
		}
	}

	void ClearTickFilters()
	{
		for ( Uint32 i = 0; i < ( Uint32 ) UpdateTickGroup::COUNT; ++i )
		{
			for ( Node& node : m_updateNodes[ i ] )
			{
				node.SetActive( true );
			}
		}
	}

#ifdef USE_PROFILER
#define SYSTEM_UPDATER_PROFILING_JOB( name, index, block ) \
	builder.DispatchJob( name, [this, index]( const job::RunContext& ) \
	{ \
		block \
	} )
#else 
// [skperf] round 55 tick probe (?perfprobe=1, probeMask bit 1 = job probes): the engine's own begin/end hooks of every
// tick group, entity bucket and bucket phase dispatch a tiny timestamp job (only while probing; nothing otherwise), so
// the game update's wall time is split per group/phase: 'CP2077 skperf tick probe' every ~2 s.
#define SYSTEM_UPDATER_PROFILING_JOB( name, index, block ) \
	if ( cpTickProbe::On() ) builder.DispatchJob( name, [index]( const job::RunContext& ) { cpTickProbe::Mark( name, Uint32( index ) ); } )
#endif

	void Internal_LinkGroups( job::Builder& builder, const UpdateContext& updateContext, Uint32 begin, Uint32 end )
	{
		for ( Uint32 igroup = begin; igroup < end; ++igroup )
		{
			SYSTEM_UPDATER_PROFILING_JOB( "BeginGroup", igroup, 
			{
				PC_BEGIN_GROUP( *m_groupInstrumentation[igroup].Get() );
				RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_BEGIN_TICK_GROUP( igroup );
			} );

			const auto& nodes = m_updateNodes[igroup];
			for ( const Node& node : nodes )
			{
				if ( node.IsActive() )
				{
					node.LinkJob_NoFence( builder, updateContext );
				}
			}
			builder.DispatchFenceExplicitly();

			SYSTEM_UPDATER_PROFILING_JOB( "EndGroup", igroup, 
			{
				RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_END_TICK_GROUP();
				PC_END_GROUP( *m_groupInstrumentation[igroup].Get() );
			} );
		}
	}

	void Internal_LinkBuckets( job::Builder& builder, const UpdateContext& updateContext )
	{
		for ( Uint32 ibucket = UpdateBucket::Vehicle; ibucket < UpdateBucket::Count; ++ibucket )
		{
			SYSTEM_UPDATER_PROFILING_JOB( "BeginBucket", ibucket, 
			{
				PC_SYNC_POINT_INST_OBJ( *m_bucketInstrumentation[ibucket].Get() );
				PC_BEGIN_GROUP( *m_bucketInstrumentation[ibucket].Get() );
				RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_BEGIN_TICK_BUCKET( ibucket );
			} );

			for ( Uint32 iphase = 0; iphase < (Uint32)UpdateBucketPhase::COUNT; ++iphase )
			{
				const NodeSpan nodes = m_bucketSpans[iphase];
				if ( !nodes.Empty() )
				{
					SYSTEM_UPDATER_PROFILING_JOB( "BeginPhase", iphase, 
					{
						PC_SYNC_POINT_INST_OBJ( *m_phaseInstrumentation[iphase].Get() );
						RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_BEGIN_TICK_PHASE( iphase );
					} );

					for ( const Node& node : nodes )
					{
						if ( node.IsActive() && node.IsInBucket( (UpdateBucket::Enum)ibucket ) )
						{
							node.LinkBucketJob_NoFence( builder, (UpdateBucket::Enum)ibucket, updateContext );
						}
					}
					builder.DispatchFenceExplicitly();

					SYSTEM_UPDATER_PROFILING_JOB( "EndPhase", iphase, 
					{
						RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_END_TICK_PHASE();
					} );
				}
			}

			SYSTEM_UPDATER_PROFILING_JOB( "EndBucket", ibucket, 
			{
				RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_END_TICK_BUCKET();
				PC_END_GROUP( *m_bucketInstrumentation[ibucket].Get() );
			} );
		}
	}

	void BuildJobChain( job::Builder& builder, const UpdateContext& updateContext )
	{
		Internal_LinkGroups ( builder, updateContext, (Uint32)UpdateTickGroup::FrameBegin, (Uint32)UpdateTickGroup::Internal_Buckets );
		Internal_LinkBuckets( builder, updateContext );
		Internal_LinkGroups ( builder, updateContext, (Uint32)UpdateTickGroup::Internal_Buckets + 1, (Uint32)UpdateTickGroup::COUNT );
	}

	void Register( const SystemUpdateNodeSetup& setup )
	{
		// #tbd: not more than once with same system/func...
		auto& group = m_updateNodes[ ( Uint32 ) setup.m_hook.group ];
		group.EmplaceBack( setup );
	}

	void Dump( const DumpContext context, const Bool isSystemTick ) const
	{
		const char* title = nullptr;
		switch ( context )
		{
			case DumpContext::Singleplayer: title = "Singleplayer"; break;
			case DumpContext::Server: title = isSystemTick ? "Server side: regular tick" : "Server side: specific client tick"; break;
			case DumpContext::Client: title = isSystemTick ? "Client side: regular tick" : "Client side: local player correction"; break;
		}
		RED_LOG( "  %s", title );

		for ( Uint32 i = 0; i < ( Uint32 ) UpdateTickGroup::COUNT; ++i )
		{
			Bool isAnyNodeActive = false;
			for ( const auto& node : m_updateNodes[ i ] )
			{
				if ( !node.IsActive() )
				{
					continue;
				}
				isAnyNodeActive = true;
			}
			if ( !isAnyNodeActive )
			{
				continue;
			}

			RED_LOG( "    %s", helper::GetSyncGroupName( ( UpdateTickGroup ) i ) );
			for ( const auto& node : m_updateNodes[ i ] )
			{
				if ( !node.IsActive() )
				{
					continue;
				}
				RED_LOG( "      %s", node.GetDebugName() );
			}
		}
	}

public:
    using NodeArray = red::DynArray< Node >;
    using NodeSpan = red::ArraySpan< Node >;

    red::StaticArray< NodeArray, (Uint32)UpdateTickGroup::COUNT > m_updateNodes;
    red::StaticArray< NodeSpan, (Uint32)UpdateBucketPhase::COUNT> m_bucketSpans;

	red::StaticArray< red::UniquePtr< red::InstrumentationObject >, (Uint32)UpdateTickGroup::COUNT > m_groupInstrumentation;
    red::StaticArray< red::UniquePtr< red::InstrumentationObject >, (Uint32)UpdateBucketPhase::COUNT > m_phaseInstrumentation;
    red::StaticArray< red::UniquePtr< red::InstrumentationObject >, UpdateBucket::Count > m_bucketInstrumentation;
};

SystemsUpdater::SystemsUpdater()
	: m_isLinked( false )
{
	m_updaterImpl = red::CreateUniquePtr< Updater >();

#ifdef RED_USE_IMGUI
	m_debugHelper = red::CreateUniquePtr< DebugHelper >();
#endif

}

SystemsUpdater::~SystemsUpdater()
{
}

void SystemsUpdater::RegisterSystemUpdate( const SystemUpdateNodeSetup& setup )
{
    RED_FATAL_ASSERT( !m_isLinked, "Cannot register update function when already linked!" );
    m_updaterImpl->Register( setup );
}

void SystemsUpdater::Link()
{
    RED_FATAL_ASSERT( !m_isLinked, "Cannot link multiple times!" );

    Updater::NodeArray& bucketNodes = m_updaterImpl->m_updateNodes[(Uint32)UpdateTickGroup::Internal_Buckets];

	// sort by phase and bucket mask (nodes with the same phase have to placed be next to each other)
    std::sort( bucketNodes.Begin(), bucketNodes.End(), []( const SystemsUpdater::Node& a, const SystemsUpdater::Node& b )
    {
        const Uint64 phaseA = (Uint64)a.GetBucketPhase();
        const Uint64 phaseB = (Uint64)b.GetBucketPhase();
        const Uint64 maskA = (Uint64)a.GetBucketMask();
        const Uint64 maskB = (Uint64)b.GetBucketMask();

        const Uint64 hashA = phaseA << 32 | maskA;
        const Uint64 hashB = phaseB << 32 | maskB;

        return hashA < hashB;
    } );

    m_updaterImpl->m_bucketSpans.Resize( (Uint32)UpdateBucketPhase::COUNT );

    // create spans for bucket phases
    for( Uint32 iphase = 0; iphase < (Uint32)UpdateBucketPhase::COUNT; ++iphase )
    {
        auto foundIt = std::find_if( bucketNodes.Begin(), bucketNodes.End(), [iphase]( const Node& node )
        {
            return (Uint32)node.GetBucketPhase() == iphase;
        } );

        if( foundIt == bucketNodes.End() )
            continue;

        auto begin = foundIt;
		auto end = begin + 1;
        while( end != bucketNodes.End() && (Uint32)end->GetBucketPhase() == iphase )
            ++end;

        m_updaterImpl->m_bucketSpans[iphase] = red::MakeArraySpan( &*begin, static_cast<Uint32>( end - begin ) );
    }

    m_isLinked = true;
}

void SystemsUpdater::Clear()
{
	for ( auto& updateNode : m_updaterImpl->m_updateNodes )
	{
		updateNode.Clear();
	}

	m_updaterImpl->m_bucketSpans.Clear();
	m_isLinked = false;
}

void SystemsUpdater::SetTickFilters( const TTickFilters& enabledClassTypes, const TTickFilters& exceptClassTypes /*= TTickFilters()*/ )
{
	m_updaterImpl->SetTickFilters( enabledClassTypes, exceptClassTypes );
}

void SystemsUpdater::ModTickFilters( const TTickFilters& modifiedClassTypes, Bool enabled )
{
	m_updaterImpl->ModTickFilters( modifiedClassTypes, enabled );
}

void SystemsUpdater::ClearTickFilters()
{
	m_updaterImpl->ClearTickFilters();
}

void SystemsUpdater::BuildJobChain( job::Builder& builder, const Float delta, const tick::Info& tickInfo )
{
	RED_FATAL_ASSERT( m_isLinked, "Building job chain before systems linked!" );
	m_updaterImpl->BuildJobChain( builder, UpdateContext( delta, tickInfo, net::PeerID::Invalid ) );
}

void SystemsUpdater::BuildJobChainAndFlush( const char* debugName, Float timeDelta, const tick::Info& tickInfo )
{
	job::Builder builder;
	BuildJobChain( builder, timeDelta, tickInfo );
	auto counter = builder.ExtractWaitCounter();
	job::FlushCounter( counter );
}
void SystemsUpdater::Dump( const DumpContext context ) const
{
	RED_LOG( "=== Systems Updater Dump (BEGIN) ===" );
	m_updaterImpl->Dump( context, true );
	RED_LOG( "=== Systems Updater Dump (END) ===" );
}

//
// Debugging stuff
//
class SystemsUpdater::DebugHelper
{
	RED_USE_MEMORY_POOL( red::PoolDebug );
public:
#ifdef RED_USE_IMGUI
	struct Draw
	{
		static void InitColumns( red::ImGui::ColumnHelper& columns )
		{
			columns.Add( 300 ).Add( 300 ).Add( 125 ).Begin();
		}

		static void Header()
		{
			ImGui::Separator();

			red::ImGui::ColumnHelper columns;
			InitColumns( columns );

			const Uint32 color = Color::LIGHT_GREEN().ToUint32();
			const ImVec4 color4 = ImGui::ColorConvertU32ToFloat4( color );

			ImGui::TextColored( color4, "FUNCTION NAME" );
			columns.Next();
			ImGui::TextColored( color4, "SYSTEM NAME" );
			columns.Next();
			ImGui::TextColored( color4, "ACTIVE" );
			columns.Next();

			columns.End();

			ImGui::Separator();
		}
		static void Item( const Node& node, const Color c )
		{
			const ImVec4 color = ImGui::ColorConvertU32ToFloat4( c.ToUint32() );

			red::ImGui::ColumnHelper columns;
			InitColumns( columns );

			ImGui::TextColored( color, node.GetDebugName() );
			columns.Next();

			ImGui::TextColored( color, node.GetNodeClassType()->GetName().AsChar() );
			columns.Next();

			{
				const Uint32 textColor = (node.IsActive()) ? Color::GREEN().ToUint32() : Color::RED().ToUint32();
				ImGui::TextColored( ImGui::ColorConvertU32ToFloat4( textColor ), node.IsActive() ? "true" : "false" );
				columns.Next();
			}

			columns.End();
			ImGui::Separator();
		}
	};

	struct FilterResult
	{
		red::BitSet64<128> indexMask;
		Uint32 bucketMask = 0;
		Uint32 phaseMask = 0;
	};
	struct Filter
	{
		static Bool Find( FilterResult* result, const Updater::NodeSpan& nodes, const AnsiChar* pattern )
		{
			if ( !pattern )
				return false;

			for ( Uint32 index = 0; index < nodes.Size(); ++index )
			{
				const Node& node = nodes[index];
				if ( node.IsActive() && Filter::Match( node.GetNodeClassType()->GetName().AsChar(), pattern ) )
				{
					result->indexMask.Set( index );
					result->bucketMask |= node.GetBucketMask();
					result->phaseMask |= RED_FLAG( (Uint32)node.GetBucketPhase() );
				}
			}

			return result->indexMask.IsAnySet();
		}

		static Bool Match( const AnsiChar* text, const AnsiChar* pattern )
		{
			return pattern ? red::Strstr( text, pattern ) != nullptr : false;
		}
	};
#endif

	static constexpr Uint32 c_filterTxtLen = 127;
	static constexpr Uint32 c_filterTxtSize = c_filterTxtLen + 1;
	AnsiChar m_systemFilterName[c_filterTxtSize] = {};
};

void SystemsUpdater::RenderDebug()
{
	if ( !m_isLinked )
		return;

#ifdef RED_USE_IMGUI
	if ( red::ImGui::IsWindowRequested( RED_NAME_CONSTEXPR( "SystemsUpdater" ) ) )
	{
		if ( ImGui::Begin( "SystemsUpdater", nullptr, ImGuiWindowFlags_HorizontalScrollbar ) )
		{
			const Uint32 defaultNodeFlags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;

			ImGui::InputText( "System filter", m_debugHelper->m_systemFilterName, DebugHelper::c_filterTxtSize );
			const AnsiChar* systemFilter = ( red::Strlen( m_debugHelper->m_systemFilterName ) > 0 ) ? m_debugHelper->m_systemFilterName : nullptr;

			if ( ImGui::TreeNodeEx( "TickGroups", ImGuiTreeNodeFlags_DefaultOpen | defaultNodeFlags ) )
			{
				for ( Uint32 igroup = (Uint32)UpdateTickGroup::FrameBegin; igroup < (Uint32)UpdateTickGroup::COUNT; ++igroup )
				{
					Updater::NodeArray& groupNodes = m_updaterImpl->m_updateNodes[igroup];
					
					DebugHelper::FilterResult groupfilterResult;
					const Bool foundAny = DebugHelper::Filter::Find( &groupfilterResult, groupNodes, systemFilter );
					
					const Uint32 treeNodeFlags = ( foundAny ) ? ImGuiTreeNodeFlags_DefaultOpen : 0;
					if ( ImGui::TreeNodeEx( helper::GetSyncGroupName( (UpdateTickGroup)igroup ), treeNodeFlags | defaultNodeFlags ) )
					{
						if ( igroup != (Uint32)UpdateTickGroup::Internal_Buckets )
						{
							DebugHelper::Draw::Header();
							for( Uint32 inode = 0; inode < groupNodes.Size(); ++inode )
							{
								const Node& node = groupNodes[inode];
								const Bool filterMatch = groupfilterResult.indexMask.Get( inode );
								const Color color = (filterMatch) ? Color::RED() : Color::WHITE();
								
								DebugHelper::Draw::Item( node, color );
							}
						}
						else
						{
							for ( Uint32 ibucket = UpdateBucket::Vehicle; ibucket < UpdateBucket::Count; ++ibucket )
							{
								Uint32 bucketNodeTreeFlags = defaultNodeFlags;
								if( groupfilterResult.bucketMask & RED_FLAG(ibucket) )
									bucketNodeTreeFlags |= ImGuiTreeNodeFlags_DefaultOpen;

								if ( ImGui::TreeNodeEx( UpdateBucket::name[ibucket], bucketNodeTreeFlags ) )
								{
									for ( Uint32 iphase = 0; iphase < (Uint32)UpdateBucketPhase::COUNT; ++iphase )
									{
										const Updater::NodeSpan phaseNodes = m_updaterImpl->m_bucketSpans[iphase];
										Uint32 phaseNodeTreeFlags = defaultNodeFlags;

										DebugHelper::FilterResult phaseFilterResult;
										DebugHelper::Filter::Find( &phaseFilterResult, phaseNodes, systemFilter );
										if ( phaseFilterResult.phaseMask & RED_FLAG( iphase ) &&
											 phaseFilterResult.bucketMask & RED_FLAG( ibucket ) )
										{
											phaseNodeTreeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
										}

										if ( ImGui::TreeNodeEx( helper::GetBucketPhaseName( (UpdateBucketPhase)iphase ), phaseNodeTreeFlags ) )
										{
											DebugHelper::Draw::Header();
											for ( const Node& node : phaseNodes )
											{
												if ( node.IsInBucket( (UpdateBucket::Enum)ibucket ) )
												{
													const Bool filterMatch = DebugHelper::Filter::Match( node.GetNodeClassType()->GetName().AsChar(), systemFilter );
													const Color color = (filterMatch) ? Color::RED() : Color::WHITE();
													DebugHelper::Draw::Item( node, color );
												}
											}

											ImGui::TreePop();
										}
									} // for
									ImGui::TreePop();
								}
							} // for
						} // if{}else{}
						ImGui::TreePop();
					}
				} // for
				ImGui::TreePop();
			}
		}
		ImGui::End();
	}
#endif
}
