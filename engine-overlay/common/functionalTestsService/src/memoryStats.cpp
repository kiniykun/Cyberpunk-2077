/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "memoryStats.h"
#include "poolsWalker.h"

#include "../../resource/include/resourceApi.h"
#include "../../resource/include/jsonSerializer.h"
#include "../../redContainers/include/string/stringUtils.h"

#ifdef RED_PLATFORM_DURANGO
#include <xmem.h>
#endif

#ifdef RED_PLATFORM_WINPC
#include <psapi.h>
#include <DXGI1_3.h>
#endif

using RapidJsonWriter = rapidjson::PrettyWriter< serialization::json::JsonTextWriter, rapidjson::UTF8< red::AnsiChar >, rapidjson::UTF8< red::AnsiChar >, res::json::JSONAllocator >;
using RapidJsonDocument = rapidjson::GenericDocument< rapidjson::UTF8< red::AnsiChar >, rapidjson::MemoryPoolAllocator< res::json::JSONAllocator >, res::json::JSONAllocator >;
using RapidJsonValue = rapidjson::GenericValue< rapidjson::UTF8< red::AnsiChar >, rapidjson::MemoryPoolAllocator< res::json::JSONAllocator > >;

using FunctionalTests::MemoryStats;
using namespace FunctionalTests;
using namespace FunctionalTestsMessage;

namespace Config
{
	extern TConfigVar< Bool > cvFunctionalTestsDumpInteropProcessingTimes;
}

MemoryStats::MemoryStats(EngineStatsProvider* engineStatsProvider) :
	m_engineStatsProvider(engineStatsProvider),
	m_dumpProcessingTimes(false),
	m_dispatcher(nullptr)
{
	RED_ASSERT(m_engineStatsProvider, "Must be initialized with valid EngineStatsProvider instance");

	FT_LOG_DEBUG("%hs", __FUNCTION__);
	m_poolsWalker = red::CreateUniquePtr< PoolsWalker >();
	m_dumpProcessingTimes = Config::cvFunctionalTestsDumpInteropProcessingTimes.Get();
}

MemoryStats::~MemoryStats()
{
	FT_LOG_DEBUG("%hs", __FUNCTION__);
}

void MemoryStats::Update(Uint64 engineTick, const red::String& playerPosition, const red::String& playerOrientaiton)
{
	red::Timer timer;
	double splitTime = 0.;

	m_data.m_engineTick = engineTick;
	red::memory::GetRuntimeMetrics(m_data);
	m_data.m_lastTimeDelta = m_engineStatsProvider->GetLastTimeDeltaUnclamped();
	m_data.m_engineTime = (Double)m_engineStatsProvider->GetRawEngineTime();

	//synchronize access to binary serializer
	{
		red::ScopedLock<red::SpinLock> lock(m_serializerLock);
		if (m_binarySerializer && ShouldSave())
		{
			red::DateTime now;
			red::Clock::GetInstance().GetLocalTime(now);
			m_data.m_rawLocalTime = now.GetRaw();

			m_data.m_poolsCurrentInfo.Clear();

			if (m_dumpProcessingTimes)
				splitTime = timer.GetSeconds();
			this->GetPoolDynamicData(
				m_data.m_poolsRuntimeInfo,
				m_data.m_poolsCurrentInfo,
				*m_poolsWalker
			);
			if (m_dumpProcessingTimes)
			{
				double elapsed{ timer.GetSeconds() };
				FT_LOG_DEBUG("Memory pools dynamic data acquired in %.2f ms", (elapsed - splitTime) * 1000.);
				splitTime = elapsed;
			}

			m_data.m_playerPosition    = playerPosition;
			m_data.m_playerOrientation = playerOrientaiton;
			m_data.m_testName          = m_testName;
			m_data.m_testGroupName     = m_testGroupName;

			FT_LOG_DEBUG(
				"Tick: %u, test:[%hs], group:[%hs] saving %u current pool infos and %u runtime data",
				engineTick,
				m_testName.AsChar(),
				m_testGroupName.AsChar(),
				m_data.m_poolsCurrentInfo.Size(),
				m_data.m_poolsRuntimeInfo.Size());

			if (m_dumpProcessingTimes)
			{
				splitTime = timer.GetSeconds();
			}

			bool hasData = true;
			m_binarySerializer->Save(hasData);
			m_binarySerializer->Save(&m_data);

			if (m_dumpProcessingTimes)
			{
				FT_LOG_DEBUG(
					"Memory stats serialized in %.2f ms",
					(timer.GetSeconds() - splitTime) * 1000.);
			}
		}
	}

	if (m_dumpProcessingTimes)
		FT_LOG_DEBUG("MemoryStats::Update() finished in %.2f ms", timer.GetSeconds() * 1000.);
}

void
MemoryStats::GetPoolDynamicData(
	red::DynArray < FunctionalTestsData::MemoryPoolRuntimeData >& runtimeData,
	red::DynArray < FunctionalTestsData::MemoryPoolStaticData >& staticData,
	PoolsWalker& poolsWalker )
{
	const bool updateNeeded{ poolsWalker.StaticUpdateNeeded() };

	if (updateNeeded)
		poolsWalker.UpdateStaticInfo();//get structure

	poolsWalker.UpdateRuntimeInfo();//update info for each pool
	poolsWalker.GetPoolsRuntimeInfo(runtimeData);//get info for each pool

	//do we have some new data ?
	if (updateNeeded)
	{
		FT_LOG_DEBUG("Reading pools static data");
		poolsWalker.GetPoolsStaticInfo(staticData);
	}
}

const FunctionalTestsData::MemoryStatsData&
MemoryStats::Data()
{
	return m_data;
}

#define REGISTER_STATISTICS_HANDLER(MESSAGE_ID)                              \
	auto callback_##MESSAGE_ID = std::bind(                                  \
		&MemoryStats::Handle_##MESSAGE_ID,                                   \
		this,                                                                \
		std::placeholders::_1);                                              \
	registrar->RegisterStatisticsMessageHandler(StatisticsMessage::MESSAGE_ID, callback_##MESSAGE_ID);

void
MemoryStats::RegisterHandlers(FunctionTestsMessagesHandlingRegistrar* registrar)
{
	FT_LOG_DEBUG("%hs", __FUNCTION__);

	REGISTER_STATISTICS_HANDLER(StartGatherMemory);
	REGISTER_STATISTICS_HANDLER(StopGatherMemory);
	REGISTER_STATISTICS_HANDLER( DumpPerTeamCPUMemoryInfo );
	REGISTER_STATISTICS_HANDLER( GetBasicMemoryInfo );
}

void
MemoryStats::RegisterDispatcher(RawMessagesDispatcher* dispatcher)
{
	FT_LOG_DEBUG("%hs", __FUNCTION__);

	m_dispatcher = dispatcher;
}

void
MemoryStats::OnPrepareForDetach()
{
	FT_LOG_DEBUG("%hs()", __FUNCTION__);

	m_poolsWalker->OnPrepareForDetach();
}

void
MemoryStats::Handle_StartGatherMemory(FunctionalTestsMessage::RawMessagePtr& message)
{
	String saveDir;
	red::String testName;
	red::String testGroupName;
	red::Float minTimeDeltaSec;

	*message
		>> saveDir
		>> testName
		>> testGroupName
		>> minTimeDeltaSec;

	FT_LOG(
		"Functional tests: startGatherData(saveDir=%hs, testName=%hs, testGroupName=%hs, minTimeDeltaSec=%.2f sec)",
		saveDir.AsChar(),
		testName.AsChar(),
		testGroupName.AsChar());
	red::String failureInformation;
	red::String path = StartGatherData(saveDir, &failureInformation, testName, testGroupName, minTimeDeltaSec);
	if (false == failureInformation.Empty())
	{
		message->BuildResponse()
			<< false
			<< failureInformation;
	}
	else
	{
		message->BuildResponse()
			<< true
			<< path;
	}
}

void
MemoryStats::Handle_StopGatherMemory(FunctionalTestsMessage::RawMessagePtr& message)
{
	FT_LOG("Functional tests: stopGatherData()");
	red::String failureInformation;

	if (false == StopGatherData(&failureInformation))
		message->BuildResponse()
		<< false
		<< failureInformation;
	else
		message->BuildResponse()
		<< true;
}
//https://en.cppreference.com/w/cpp/algorithm/lower_bound
template<typename T>
bool BinaryFind( red::DynArray<T>& data, const red::String& key, red::Uint32& position )
{
	auto hash = key.CalcHash( );
	auto first = data.Begin( );
	typename std::iterator_traits<typename red::DynArray < T >::iterator >::difference_type count, step;
	count = std::distance( first, data.End( ) );
	typename red::DynArray < T >::iterator it;

	while( count > 0 ) {
		it = first;
		step = count / 2;
		std::advance( it, step );
		if( it->m_poolName.CalcHash( ) < hash ) {
			first = ++it;
			count -= step + 1;
		}
		else
			count = step;
	}

	bool found = false;
	if( first != data.End( ) && first->m_poolName == key )
	{
		position = static_cast< red::Uint32 >( std::distance( data.Begin( ), first ) );
		found = true;
	}
	return found;
};

red::Uint64 TreeAllocated(
	red::DynArray < FunctionalTestsData::MemoryPoolRuntimeData >& runtimeData,
	red::DynArray < FunctionalTestsData::MemoryPoolStaticData >& staticData,
	const red::String& poolName,
	Bool isRecursion = false )
{
	red::Uint32 position = 0;
	bool found = BinaryFind( runtimeData, poolName, position );
	RED_ASSERT( found, "Failed to find matching entry in runtime data" );
	red::Uint64 memoryUsage = runtimeData[ position ].m_bytesAllocated;
	found = BinaryFind( staticData, poolName, position );
	RED_ASSERT( found, "Failed to find matching entry in static data" );

	if ( isRecursion )
	{
		if ( !staticData[ position ].m_contributeToParentMetrics )
		{
			return 0;
		}
	}

	for( const red::String& childName : staticData[ position ].m_children )
	{
		memoryUsage += TreeAllocated( runtimeData, staticData, childName, true );
	}

	return memoryUsage;
}

void
MemoryStats::Handle_DumpPerTeamCPUMemoryInfo( FunctionalTestsMessage::RawMessagePtr& message )
{
	FT_LOG_DEBUG( "%hs()", __FUNCTION__ );

	//use scoped instance to not interfere with possible ongoing memory dump
	red::DynArray < FunctionalTestsData::MemoryPoolRuntimeData > runtimeData{ red::PoolFunctionalTests( ) };
	red::DynArray < FunctionalTestsData::MemoryPoolStaticData > staticData{ red::PoolFunctionalTests( ) };
	PoolsWalker poolsWalker;

	GetPoolDynamicData( runtimeData, staticData, poolsWalker );

	//prepare for binary search
	std::sort(
		runtimeData.Begin( ),
		runtimeData.End( ),
		[ ]( const FunctionalTestsData::MemoryPoolRuntimeData& lhs, const FunctionalTestsData::MemoryPoolRuntimeData& rhs )
	{
		return lhs.m_poolName.CalcHash( ) < rhs.m_poolName.CalcHash( );
	} );

	std::sort(
		staticData.Begin( ),
		staticData.End( ),
		[ ]( const FunctionalTestsData::MemoryPoolStaticData& lhs, const FunctionalTestsData::MemoryPoolStaticData& rhs )
	{
		return lhs.m_poolName.CalcHash( ) < rhs.m_poolName.CalcHash( );
	} );

	RED_LOG_DEBUG( "runtime data count: [%u]", runtimeData.Size( ) );
	for( const FunctionalTestsData::MemoryPoolRuntimeData data : runtimeData )
	{
		RED_LOG_DEBUG( "name: [%hs] bytes: [%llu]", data.m_poolName.AsChar( ), data.m_bytesAllocated );
	}

	RED_LOG_DEBUG( "static data count: [%u]", staticData.Size( ) );
	for( const FunctionalTestsData::MemoryPoolStaticData data : staticData )
	{
		RED_LOG_DEBUG( "name: [%hs] bytes: [%lld]", data.m_poolName.AsChar( ), data.m_budget );
	}

	using RapidJsonWriter = rapidjson::PrettyWriter<
		serialization::json::JsonTextWriter,
		rapidjson::UTF8< red::AnsiChar >,
		rapidjson::UTF8< red::AnsiChar >,
		res::json::JSONAllocator >;

	String output;
	serialization::json::JsonTextWriter textWriter{ output };
	RapidJsonWriter writer{ textWriter };

	FunctionalTestsData::MemoryStatsData data;
	red::memory::GetRuntimeMetrics( data );

	const red::String poolCPU{ "PoolCPU" };
	red::String poolName;

	writer.StartObject( );

	writer.Key( "totalValue" );
	writer.Uint64( data.cpuBytesAllocated /*TreeAllocated( runtimeData, staticData, poolCPU )*/ );

	writer.Key( "totalGoal" );
#if 0
	{
		red::Uint32 position;
		bool found = BinaryFind( staticData, poolCPU, position );
		RED_ASSERT( found, "PoolCPU not found! Wat?" );
		writer.Uint64( staticData[ position ].m_budget + staticData[ position ].m_childrenBudget );
	}
#else
	writer.Uint64( data.cpuPoolBudget );
#endif

	writer.Key( "teams" );
	writer.StartArray( );
	
	for( const FunctionalTestsData::MemoryPoolStaticData data : staticData )
	{
		if( data.m_parent != poolCPU )
			continue;

		writer.StartObject( );

		writer.Key( "name" );
		poolName = data.m_poolName;
		poolName.Replace( "Pool", "" );
		writer.String( poolName.AsChar( ) );

		writer.Key( "goal" );
		writer.Int64( data.m_budget );

		writer.Key( "value" );
		writer.Uint64( TreeAllocated( runtimeData, staticData, data.m_poolName ) );

		writer.Key( "children" );
		writer.StartArray();

		for ( const String child : data.m_children )
		{
			Uint32 position = 0;
			bool found = BinaryFind( staticData, child, position );
			RED_ASSERT( found, "Failed to find matching entry in static data" );

			writer.StartObject();

			writer.Key( "name" );
			writer.String( child.AsChar() );

			writer.Key( "value" );
			writer.Uint64( TreeAllocated( runtimeData, staticData, child, true ) );

			writer.Key( "goal" );
			writer.Int64( staticData[position].m_budget );

			writer.EndObject();
		}

			writer.EndArray();

		writer.EndObject( );
	}

	writer.EndArray( );

	writer.EndObject( );

	message->BuildResponse( )
		<< true
		<< output;
}

void
MemoryStats::Handle_GetBasicMemoryInfo( FunctionalTestsMessage::RawMessagePtr& message )
{
	FT_LOG_DEBUG( "%hs()", __FUNCTION__ );

	red::String errorInfo;
	BasicMemoryInfo memoryCPU, memoryGPU;
	bool result = this->GetMemoryInfo( errorInfo, memoryCPU, memoryGPU );
	if( false == result )
	{
		message->BuildResponse( )
			<< result
			<< errorInfo;
	}
	else
	{
		message->BuildResponse( )
			<< result
			<< memoryCPU.m_usedPhysicalMemory
			<< memoryCPU.m_availablePhysicalMemory
			<< memoryCPU.m_totalPhysicalMemory
			<< memoryGPU.m_usedPhysicalMemory
			<< memoryGPU.m_availablePhysicalMemory
			<< memoryGPU.m_totalPhysicalMemory;
	}
}

using FunctionalTests::BasicMemoryInfo;

#ifdef RED_PLATFORM_WINPC
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
);
#endif

#ifdef RED_PLATFORM_DURANGO
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
);
#endif

#ifdef RED_PLATFORM_ORBIS
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
);
#endif

#ifdef RED_PLATFORM_LINUX
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
);
#endif

bool
MemoryStats::GetMemoryInfo(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
)
{
	bool result = true;
	red::Memzero( &memoryCPU, sizeof( BasicMemoryInfo ) );
	red::Memzero( &memoryGPU, sizeof( BasicMemoryInfo ) );

	//UNDONE: per OS info from API
	#ifdef RED_PLATFORM_WEB
    errorInfo = "Physical CPU/GPU memory statistics are unavailable in the browser";
    return false;
#else
    return GetMemoryInfoInternal( errorInfo, memoryCPU, memoryGPU );
#endif
}

#ifdef RED_PLATFORM_WINPC
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
)
{
	bool result = true;

	MEMORYSTATUS status;
	status.dwLength = sizeof( status );
	GlobalMemoryStatus( &status );
	memoryCPU.m_totalPhysicalMemory = static_cast< red::Uint64 >( status.dwTotalPhys );
	memoryCPU.m_availablePhysicalMemory = static_cast< red::Uint64 >( status.dwAvailPhys );

	PROCESS_MEMORY_COUNTERS pmc;
	HANDLE hProcess = GetCurrentProcess( );
	if( hProcess == INVALID_HANDLE_VALUE )
	{
		errorInfo = "Failed to obtain self pseudo HANDLE";
		result = false;
	}
	else
	{
		if( !GetProcessMemoryInfo( hProcess, &pmc, sizeof( pmc ) ) )
		{
			errorInfo = red::String::Printf( "Failed to obtain memory info (through GetProcessMemoryInfo), error: %u", ::GetLastError( ) );
			result = false;
		}
		else
		{
			memoryCPU.m_usedPhysicalMemory = static_cast< red::Uint64 >( pmc.WorkingSetSize );
		}
		//CloseHandle( hProcess );
	}

	memoryGPU.m_availablePhysicalMemory = 0;//undone
	memoryGPU.m_totalPhysicalMemory = 0;//undone
	memoryGPU.m_usedPhysicalMemory = 0;//undone

#if 0
	//DXGI_QUERY_VIDEO_MEMORY_INFO
	HRESULT QueryVideoMemoryInfo(
		UINT                         NodeIndex,
		DXGI_MEMORY_SEGMENT_GROUP    MemorySegmentGroup,
		DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo
	);
#endif

	return result;
}
#endif

#ifdef RED_PLATFORM_DURANGO
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
)
{
	TITLEMEMORYSTATUS titleMemoryStatus;
	std::memset( &titleMemoryStatus, 0, sizeof( TITLEMEMORYSTATUS ) );
	titleMemoryStatus.dwLength = sizeof( titleMemoryStatus );
	BOOL result = TitleMemoryStatus( &titleMemoryStatus );

	if( result == 0 )
	{
		errorInfo = red::String::Printf( "Cannot fetch Durango available memory, error: %u", ::GetLastError( ) );
		result = false;
	}
	else
	{
		memoryCPU.m_totalPhysicalMemory = titleMemoryStatus.ullTotalMem;
		memoryCPU.m_availablePhysicalMemory = titleMemoryStatus.ullAvailMem;
		memoryCPU.m_usedPhysicalMemory = titleMemoryStatus.ullTitleUsed;
	}

	memoryGPU.m_availablePhysicalMemory = 0;//undone
	memoryGPU.m_totalPhysicalMemory = 0;//undone
	memoryGPU.m_usedPhysicalMemory = 0;//undone

	return result;
}
#endif

#ifdef RED_PLATFORM_ORBIS
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
)
{
	bool result = true;

	off_t offset = 0;
	size_t size;

	int32_t sceResult = sceKernelAvailableDirectMemorySize(
		0,
		SCE_KERNEL_MAIN_DMEM_SIZE,//macro
		0,
		&offset,
		&size
	);

	RED_MEMORY_ASSERT( sceResult == SCE_OK, "SYSTEM ERROR cannot fetch Orbis available memory." );
	RED_UNUSED( sceResult );

	memoryCPU.m_availablePhysicalMemory = static_cast< red::Uint64 >( size );
	memoryCPU.m_totalPhysicalMemory = static_cast<red::Uint64>( SCE_KERNEL_MAIN_DMEM_SIZE );//macro
	memoryCPU.m_usedPhysicalMemory = 0;

	memoryGPU.m_availablePhysicalMemory = 0;//undone
	memoryGPU.m_totalPhysicalMemory = 0;//undone
	memoryGPU.m_usedPhysicalMemory = 0;//undone

	return result;
}
#endif

#ifdef RED_PLATFORM_LINUX
static bool
GetMemoryInfoInternal(
	red::String& errorInfo,
	BasicMemoryInfo& memoryCPU,
	BasicMemoryInfo& memoryGPU
)
{
	RED_UNUSED2( memoryCPU, memoryGPU );

	errorInfo = "Not there yet!";
	return false;
}
#endif
