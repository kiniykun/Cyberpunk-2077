/*
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/
//owner: bartlomiej.walecki

#include "build.h"
#include <cstdio>

#include "communityTimePeriodsManager.h"
#include "communitySpawnScheduler.h"

#include "../../world/include/gameInstance.h"

namespace game
{

//------------------------------------------------------------------------------------------------------------------
CommunityTimePeriodsManager::CommunityTimePeriodsManager() = default;

//------------------------------------------------------------------------------------------------------------------
CommunityTimePeriodsManager::~CommunityTimePeriodsManager() = default;

//------------------------------------------------------------------------------------------------------------------
void CommunityTimePeriodsManager::Initialize( const IGameInstance& gameInstance )
{
	RED_ASSERT( m_timeSystem == nullptr, "double init?" );
	m_timeSystem = &gameInstance.GetSystem< ITimeSystem >();

	m_currHour = m_timeSystem->GetGameTime().Hours();
	m_newHour = -1;
}

//------------------------------------------------------------------------------------------------------------------
void CommunityTimePeriodsManager::OnStreamingWorldLoaded()
{
	// CP2077 web [nomadplay]: loading a save reaches this before Initialize() (m_timeSystem == nullptr) and trapped
	// with 'memory access out of bounds'. Initialize() reads the hour itself, so skipping here is safe.
	if ( m_timeSystem == nullptr )
	{
		std::fprintf( stderr, "[nomadplay] CommunityTimePeriodsManager::OnStreamingWorldLoaded before Initialize - skipped\n" );
		return;
	}
	m_currHour = m_timeSystem->GetGameTime().Hours();
}

//------------------------------------------------------------------------------------------------------------------
void CommunityTimePeriodsManager::Shutdown()
{
	for( TimePeriodHourEntry& hourEntry : m_timePeriodHours )
	{
		RED_ASSERT( hourEntry.m_schedulersToInform.Empty(), "all schedulers should be unregistered already" );
		RED_ASSERT( hourEntry.m_callbackId == 0, "all callbacks should be unregistered already" );

		hourEntry.m_schedulersToInform.Clear();
		hourEntry.m_callbackId = 0;
	}
	m_newHour = -1;
	
	m_timeSystem = nullptr;
}

//------------------------------------------------------------------------------------------------------------------
void CommunityTimePeriodsManager::Update()
{
	if( m_newHour >= 0 )
	{
		Uint64 frameNumber = GEngine->GetCurrentEngineTick();
		
		Uint32 hour = Uint32( m_newHour );
		while( hour != m_currHour )
		{
			const TimePeriodHourEntry& hourEntry = m_timePeriodHours[ hour ];
			for( CommunitySpawnScheduler* scheduler : hourEntry.m_schedulersToInform )
			{
				scheduler->OnTimePeriodReady( hour, frameNumber );
			}

			hour = ( hour == 0 ? 23 : --hour );
		}
		
		m_currHour = Uint32( m_newHour );
		m_newHour = -1;
	}
}

//------------------------------------------------------------------------------------------------------------------
void CommunityTimePeriodsManager::RegisterTimePeriodsCallback( CommunitySpawnScheduler* scheduler, const red::DynArray< Uint8 >& timePeriods )
{
	RED_FATAL_ASSERT( m_timeSystem != nullptr, "system not initialized" );
	RED_ASSERT( timePeriods.Size() > 1, "attempt to register callbacks for %u timePeriods", timePeriods.Size() );
		
	for( Uint8 hour : timePeriods )
	{
		TimePeriodHourEntry& hourEntry = m_timePeriodHours[ hour ];
		if( hourEntry.m_schedulersToInform.Empty() )
		{
			auto callbackFunc = [ = ]()
			{
				OnHourHasStruck( hour );
			};
			GameTime callbackGameTime( 0, hour, 0, 0 );
			hourEntry.m_callbackId = m_timeSystem->RegisterListener( std::move( callbackFunc ), callbackGameTime );
		}

		RED_ASSERT( !hourEntry.m_schedulersToInform.Exist( scheduler ), "we're registering a scheduler twice! (hour: '%u')", hour );
		hourEntry.m_schedulersToInform.PushBack( scheduler );
	}
}

//------------------------------------------------------------------------------------------------------------------
void CommunityTimePeriodsManager::UnregisterTimePeriodsCallback( CommunitySpawnScheduler* scheduler )
{
	RED_FATAL_ASSERT( m_timeSystem != nullptr, "system not initialized" );

	//TODO - bartlomiej.walecki - algorithm below is slow, adding a parameter that would point which hours to look for would speed things up
		
	for( TimePeriodHourEntry& hourEntry : m_timePeriodHours )
	{
		if( !hourEntry.m_schedulersToInform.Empty() )
		{
			RED_ASSERT( hourEntry.m_callbackId != 0, "expected valid callback id for non-empty table with schedulers" );
				
			hourEntry.m_schedulersToInform.Remove( scheduler );
			if( hourEntry.m_schedulersToInform.Empty() )
			{
				m_timeSystem->UnregisterListener( hourEntry.m_callbackId );
				hourEntry.m_callbackId = 0;
			}
		}
	}
}

//------------------------------------------------------------------------------------------------------------------
void CommunityTimePeriodsManager::OnHourHasStruck( Uint32 hour )
{
	RED_FATAL_ASSERT( hour <= 23, "invalid hour! (%u)", hour );

	const TimePeriodHourEntry& hourEntry = m_timePeriodHours[ hour ];
	RED_ASSERT( hourEntry.m_callbackId != 0, "why are we informed about %u hour value if we don't listen to it?", hour );
	RED_ASSERT( !hourEntry.m_schedulersToInform.Empty(), "why are we listening to %u hour if no scheduler is registered?", hour );

	m_newHour = Int32( hour );
}

//------------------------------------------------------------------------------------------------------------------
Uint32 CommunityTimePeriodsManager::GetCurrentGameTimeHour() const
{
	RED_FATAL_ASSERT( m_timeSystem != nullptr, "system not initialized" );
	return m_timeSystem->GetGameTime().Hours();
}

}	//game
