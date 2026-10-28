/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "gameServicesSaveSlots.h"
#include "gameServicesTypes.h"

namespace services
{
	constexpr AnsiChar const* SlotsUtility::SLOT_NAME[];

	SlotsUtility::SlotsUtility( const SlotsConfig& cfg )
		: m_config( cfg )
		, m_slots{ PoolSaveData(), PoolSaveData(), PoolSaveData(), PoolSaveData(), PoolSaveData() }
		, m_nextSaveNumber{}
		, m_freeSaveNumbers{ PoolSaveData(), PoolSaveData(), PoolSaveData(), PoolSaveData(), PoolSaveData() }
	{
		for( Uint8 t = 0; t < SlotType::Max; ++t )
		{
			m_slots[ t ].Reserve( cfg.m_numSlots[ t ] );
		}
	}

	void SlotsUtility::Populate( GameSaveViews& existingSaves )
	{
		Clear();

		GameSaveViews unrecognizedSaves{ red::PoolFrame() };

		for ( const auto& saveView : existingSaves )
		{
			if ( saveView.name.Empty() )
			{
				RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Services, "SlotsUtility: Received save with invalid name" );
				continue;
			}

			SlotType slotType;
			Uint32 saveNumber;
			if ( RecognizeName( saveView.name, slotType, saveNumber ) )
			{
				RED_LOG_CATEGORY_INFO( red::LoggerCategory_Services, "SlotsUtility: Correctly recognized save name '%s'", saveView.name.AsChar() );

				MarkSaveNumberUsed( slotType, saveNumber, saveView.date );

				TakeFreeSlot( slotType, saveView );
			}
			else
			{
				RED_LOG_CATEGORY_INFO( red::LoggerCategory_Services, "SlotsUtility: not recognized save name '%s'", saveView.name.AsChar() );
				// Currently we don't want to handle unrecognized saves. Uncomment this support them.
				//unrecognizedSaves.PushBack( saveView );
			}
		}

		for ( const auto& saveView : unrecognizedSaves )
		{
			TakeFreeSlot( ManualSave, saveView );
			RED_LOG_CATEGORY_INFO( red::LoggerCategory_Services, "SlotsUtility: Added unrecogniezed save '%s' as manual save", saveView.name.AsChar() );
		}

		m_populatedOn = EngineTime::GetNow();
	}

	Bool SlotsUtility::FindFreeSlot( SlotType type, SlotInfo*& freeSlot, red::String& name, Bool& willOverwrite )
	{
		if ( !m_populatedOn.IsValid() )
		{
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Services, "SlotsUtility::SelectSlot: FindFreeSlot() has to be called after Populate()" );
			return false;
		}

		const EngineTime now = EngineTime::GetNow();
		const Float secondsPassedSinceLastPopulateCall = Float( now - m_populatedOn );
		if ( secondsPassedSinceLastPopulateCall > 5.f )
		{
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Services, "SlotsUtility::SelectSlot: secondsPassedSinceLastPopulateCall = %.2f... Did you call Populate() right before calling AutoSelect()?", secondsPassedSinceLastPopulateCall );
		}

		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );

		willOverwrite = false;

		Uint32 saveNumber = 0;
		Uint32 selectedIndex;
		if ( !FindFirstEmptyIndex( type, selectedIndex ) )
		{
			auto& slots = m_slots[type];
			const Uint32 numSlots = slots.Size();
			if ( IsUnlimited( type ) || !m_freeSaveNumbers[ type ].Empty() )
			{
				slots.Resize( numSlots + 1 );
				selectedIndex = numSlots;
				saveNumber = GetNextSaveNumber( type );
			}
			else
			{
				if ( !IsAutoOverwritingEnabled( type ) )
				{
					RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Services, "SlotsUtility::SelectSlot: Auto overwrite for save type '%ld' is disabled", type );
					return false;
				}

				willOverwrite = true;
				if ( !FindOldestIndex( type, selectedIndex ) )
				{
					RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Services, "SlotsUtility::SelectSlot: Failed to find oldest save for save type '%ld'", type );
					return false;
				}
			}
		}
		else
		{
			saveNumber = GetNextSaveNumber( type );
		}

		freeSlot = &m_slots[type][selectedIndex];
		freeSlot->type = type;
		if ( willOverwrite )
		{
			name = freeSlot->name;
		}
		else
		{
			name = FormatName( type, saveNumber );
			freeSlot->name = name;
		}

		return true;
	}

	Bool SlotsUtility::HasFreeSlot( SlotType type ) const
	{
		if ( !m_populatedOn.IsValid() )
		{
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Services, "SlotsUtility::SelectSlot: HasFreeSlot() has to be called after Populate()" );
			return false;
		}

		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );

		Uint32 selectedIndex;
		if ( !FindFirstEmptyIndex( type, selectedIndex ) )
		{
			if ( IsUnlimited( type ) || !m_freeSaveNumbers[ type ].Empty() )
			{
				return true;
			}
			else
			{
				return false;
			}
		}
		else
		{
			return true;
		}
	}

	Bool SlotsUtility::FindSlot( SlotType type, const red::String& name, SlotInfo*& slot )
	{
		if ( !m_populatedOn.IsValid() )
		{
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Services, "SlotsUtility::SelectSlot: FindSlot() has to be called after Populate()" );
			return false;
		}

		const EngineTime now = EngineTime::GetNow();
		const Float secondsPassedSinceLastPopulateCall = Float( now - m_populatedOn );
		if ( secondsPassedSinceLastPopulateCall > 5.f )
		{
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Services, "SlotsUtility::SelectSlot: secondsPassedSinceLastPopulateCall = %.2f... Did you call Populate() right before calling AutoSelect()?", secondsPassedSinceLastPopulateCall );
		}

		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );

		for ( Uint32 index = 0; index < m_slots[type].Size(); ++index )
		{
			SlotInfo& slotInfo = m_slots[ type ][ index ];
			if ( slotInfo.name == name )
			{
				slot = &slotInfo;
				return true;
			}
		}

		return false;
	}

	SlotType SlotsUtility::NameToSlotType( const String& str )
	{
		for ( Uint8 t = 0; t < SlotType::Max; ++t )
		{
			if ( str.BeginsWith( SLOT_NAME[t] ) )
			{
				return SlotType( t );
			}
		}

		return SlotType::ManualSave;
	}

	Bool SlotsUtility::FreeSlot( const GameSaveView& save )
	{
		SlotType slotType;
		Uint32 saveNumber;
		if ( RecognizeName( save.name, slotType, saveNumber ) )
		{
			RED_LOG_CATEGORY_INFO( red::LoggerCategory_Services, "SlotsUtility: Correctly recognized save name '%s'", save.name.AsChar() );
			FreeSaveNumber( slotType, saveNumber );
		}
		else
		{
			RED_LOG_CATEGORY_INFO( red::LoggerCategory_Services, "SlotsUtility: Not recognized save name '%s'", save.name.AsChar() );
			slotType = ManualSave;
		}

		auto& slots = m_slots[slotType];
		auto it = std::find_if( slots.Begin(), slots.End(), [id = save.id]( const SlotInfo& info ){ return info.id == id; } );
		if ( it != slots.End() )
		{
			slots.Remove( it );
			return true;
		}
		
		return false;
	}

	void SlotsUtility::Clear()
	{
		for ( Uint8 t = 0; t < SlotType::Max; ++t )
		{
			m_slots[t].Clear();
			ClearSaveNumbersState( SlotType(t) );
		}
	}

	void SlotsUtility::TakeFreeSlot( SlotType type, const GameSaveView& saveView )
	{
		Uint32 index;
		if ( !FindFirstEmptyIndex( type, index ) )
		{
			const Uint32 numSlots = m_slots[type].Size();
			m_slots[type].Resize( numSlots + 1 );
			index = numSlots;
		}

		auto& slot = m_slots[type][index];
		slot.id = saveView.id;
		slot.name = saveView.name;
		slot.date = saveView.date;
		slot.type = type;
	}

	Bool SlotsUtility::IsValid( SlotType type, Uint32 index ) const
	{
		const Uint32 limit = GetLimitOf( type );
		return index < limit || limit == 0;
	}

	Bool SlotsUtility::IsUnlimited( SlotType type ) const
	{
		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );
		return m_config.m_numSlots[type] == 0;
	}

	Uint32 SlotsUtility::GetLimitOf( SlotType type ) const
	{
		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );
		return m_config.m_numSlots[type];
	}

	Bool SlotsUtility::IsAutoOverwritingEnabled( SlotType type ) const
	{
		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );
		return m_config.m_autoOverwriting[type];
	}

	Bool SlotsUtility::FindFirstEmptyIndex( SlotType type, Uint32& outIndex ) const
	{
		auto& slots = m_slots[type];
		for ( Uint32 i = 0; i < slots.Size(); ++i )
		{
			if ( slots[i].IsEmpty() )
			{
				outIndex = i;
				return true;
			}
		}

		return false;
	}

	Uint32 SlotsUtility::GetNextSaveNumber( SlotType slotType )
	{
		if( !m_freeSaveNumbers[slotType].Empty() )
		{
			Uint32 saveNumber = m_freeSaveNumbers[ slotType ].PopBack();
			m_nextSaveNumber[ slotType ] = ::Max( m_nextSaveNumber[ slotType ], saveNumber + 1 );
			return saveNumber;
		}
		
		return m_nextSaveNumber[ slotType ]++;
	}

	void SlotsUtility::MarkSaveNumberUsed( SlotType slotType, Uint32 saveNumber, red::DateTime saveData )
	{
		m_freeSaveNumbers[ slotType ].Remove( saveNumber );
		m_nextSaveNumber[ slotType ] = ::Max( m_nextSaveNumber[ slotType ], saveNumber + 1 );
	}

	void SlotsUtility::FreeSaveNumber( SlotType slotType, Uint32 saveNumber )
	{
		const Uint32 limit = GetLimitOf( slotType );
		if( limit > 0 && saveNumber < limit )
		{
			m_freeSaveNumbers[ slotType ].PushBack( saveNumber );
			// Sort in DESC order to use smallest number first
			std::sort( m_freeSaveNumbers[ slotType ].RBegin(), m_freeSaveNumbers[ slotType ].REnd() );
		}
	}

	void SlotsUtility::ClearSaveNumbersState( SlotType slotType )
	{
		m_nextSaveNumber[ slotType ] = 0;
		for( Uint8 t = 0; t < SlotType::Max; ++t )
		{

			const Uint32 limit = GetLimitOf( SlotType( t ) );
			m_freeSaveNumbers[ t ].Clear();
			m_freeSaveNumbers[ t ].Reserve( limit );

			for ( Uint32 n = limit; n > 0; --n )
			{
				m_freeSaveNumbers[ t ].PushBack( n - 1 );
			}
		}
	}

	Bool SlotsUtility::FindOldestIndex( SlotType type, Uint32& outIndex ) const
	{
		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );

		if ( m_slots[type].Empty() )
		{
			return false;
		}

		Uint32 best = 0;
		red::DateTime maxDate;
		maxDate.SetRaw( std::numeric_limits< Uint64 >::max() );

		for ( Uint32 i = 0; i < m_slots[ type ].Size(); ++i )
		{
			if ( m_slots[type][i].date < maxDate )
			{
				maxDate = m_slots[type][i].date;
				best = i;
			}
		}

		outIndex = best;
		return true;
	}

	String SlotsUtility::FormatName( SlotType type, Uint32 index ) const
	{
		// [savetime] Browser port: "%ld" reads a 64-bit long on wasm64 (32-bit on Windows), so the Uint32 index
		// picked up garbage upper bits ("QuickSave-4294967297"). Pass a real long; the format string is unchanged.
		return String::Printf( "%s-%ld", SlotTypeToAnsiChar( type ), static_cast< long >( index ) );
	}

	Bool SlotsUtility::IsEmpty( SlotType type, Uint32 index ) const
	{
		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );
		if ( index >= m_slots[type].Size() )
		{
			return true;
		}

		return m_slots[type][index].IsEmpty();
	}

	red::DateTime SlotsUtility::GetDate( SlotType type, Uint32 index ) const
	{
		RED_FATAL_ASSERT( type < SlotType::Max, "Tried to check slot type %d, but there are only %d slot types", type, SlotType::Max );
		if ( index >= m_slots[type].Size() )
		{
			return red::DateTime();
		}

		return m_slots[type][index].date;
	}

	Bool SlotsUtility::RecognizeName( const red::String& name, SlotType& outType, Uint32& saveNumber ) const
	{
		constexpr AnsiChar c_separator = '-';
		constexpr Uint32 c_max = 32;
		
		AnsiChar namePart[c_max] = { '\0' };
		AnsiChar indexPart[c_max] = { '\0' };
		AnsiChar* currPart = namePart; 

		// parse
		Uint32 i = 0;
		for ( red::String::const_iterator it = name.Begin(); it != name.End(); ++it )
		{
			const AnsiChar c = *it;

			// eat separator
			if ( c == c_separator )
			{
				if ( currPart == namePart )
				{
					namePart[i] = '\0';  
					currPart = indexPart;
					i = 0;
					continue;
				}
				else
				{
					return false;
				}
			}
			
			// eat other char
			if ( i < c_max - 1 )
			{
				// eat index digit
				if ( currPart == indexPart )
				{
					// only digits allowed here
					if ( c <= '9' && c >= '0' )
					{
						currPart[i++] = c;
					}
					else
					{
						return false;
					}
				}
				else // eat name char
				{
					currPart[i++] = c;
				}
			}
			else
			{
				return false;
			}
		}

		if ( currPart == namePart )
		{
			return false;
		}
		else
		{
			currPart[i] = '\0';
		}

		// ok, we only have 2-part name, separated by '-', lets check both parts
		outType = SlotType::Max;
		for ( Uint8 t = 0; t < SlotType::Max; ++t )
		{
			if ( 0 == red::StrcmpNC( SLOT_NAME[t], namePart ) )
			{
				outType = SlotType( t );
				break;
			}
		}

		if ( outType == SlotType::Max )
		{
			return false;
		}

		// first part recognized, check the other one
		AnsiChar* end = indexPart;
		if ( !red::StringToInt( saveNumber, indexPart, &end, red::Base::BaseTen ) )
		{
			return false;
		}

		return end == &indexPart[i];
	}

}
