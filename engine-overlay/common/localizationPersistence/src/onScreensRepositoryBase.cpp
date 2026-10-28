#include "build.h"

#include "../include/onScreensRepositoryBase.h"
#include "../include/locRepositoryVariants.h"

#include "../../../common/redFileSystem/include/fileSys.h"
#include "../../../common/redFileSystem/include/rawFileWriter.h"
#include "../../../common/resource/include/jsonFileHelper.h"
#include "locMemory.h"
#include "../../redJobs2/include/jobBuilder.h"

namespace localizationPersistence
{
	OnScreensRepositoryBase::OnScreensRepositoryBase() = default;
	OnScreensRepositoryBase::~OnScreensRepositoryBase() = default;

	// isNeutralVariant means that requested variant is either neutral or female
	const red::String OnScreensRepositoryBase::GetStringFromHashmap( const Uint64 key, const Bool isNeutralVariant, Bool& stringFound ) const
	{
		red::String onscreensString = red::String::EMPTY();

		if ( isNeutralVariant )
		{
			stringFound = m_stringCache.Find( key, onscreensString );
		}
		else
		{
			stringFound = m_stringCacheMale.Find( key, onscreensString );
			if ( !stringFound )
			{
				stringFound = m_stringCache.Find( key, onscreensString );
			}
		}

		// [l10n] browser port: telemetry consent popup body (onscreens primaryKey 51246,
		// secondaryKey ...-system_notifications-telemetryConsentBody) starts with "Hey Choomba!".
		// Rename only that greeting; the rest of the text, every other key and other languages are unchanged.
		// Both lookup paths (primary key and secondary-key fallback) arrive here with the primary key.
		if ( stringFound && key == 51246ull && onscreensString.BeginsWith( red::String( "Hey Choomba!" ) ) )
		{
			onscreensString = red::String( "Hey Cyberpunk2077Browser!" ) + onscreensString.MidString( 12 );
		}

		return onscreensString;
	}

	const red::String OnScreensRepositoryBase::GetString( const Uint64 key, const Bool isNeutralVariant, Bool& stringFound ) const
	{
		red::String onscreensString = GetStringFromHashmap(key, isNeutralVariant, stringFound );
  		if ( !stringFound )
  		{
			Uint32 primaryKey = 0;
  			bool secondaryKeyFound = m_stringCacheOldKeys.Find( key, primaryKey );
			if( secondaryKeyFound )
				onscreensString = GetStringFromHashmap( primaryKey, isNeutralVariant, stringFound );
  		}

		return onscreensString;
	}

	void OnScreensRepositoryBase::Load( const res::ResourcePath& resPath )
	{
		if ( !m_stringCache.Empty() )
			m_stringCache.Clear();

		if ( !m_stringCacheOldKeys.Empty() )
			m_stringCacheOldKeys.Clear();

		if ( !m_stringCacheMale.Empty() )
			m_stringCacheMale.Clear();

		const auto onScreensEntryArray = localizationPersistence::SyncLoadLocRepositoryVariant( resPath );

		if ( !onScreensEntryArray )
			return;

		for ( const OnScreenEntry& entry : onScreensEntryArray->m_entries )
		{
			Uint64 dbKey = entry.m_primaryKey;

			if ( !m_stringCache.KeyExist( dbKey ) )
			{
				m_stringCache.Insert( dbKey, entry.m_femaleVariant );
				if ( entry.m_maleVariant.Length() > 0 )
					m_stringCacheMale.Insert( dbKey, entry.m_maleVariant );
			}
			else
			{
				RED_DATA_ERROR( red::LoggerCategory_Localization, false, "Duplicate OnScreen key %d", dbKey );
				continue;
			}

			if ( entry.m_secondaryKey.Length() > 0 )
			{
				Uint64 keyHash = 0;
				
				keyHash = red::CalculateHash64WithLength( entry.m_secondaryKey.AsChar(), entry.m_secondaryKey.Length() );

				if ( !m_stringCacheOldKeys.KeyExist( keyHash ) )
				{
					red::String lowCaseEntry = entry.m_secondaryKey;
					if ( lowCaseEntry.ToLower().BeginsWith( "ui-" ) || lowCaseEntry.ToLower().BeginsWith( "gameplay-" ) || lowCaseEntry.ToLower().BeginsWith( "common-" ) )
					{
						String uppercaseKey = entry.m_secondaryKey.AsChar();
						uppercaseKey.ToUpper();
						m_stringCacheOldKeys.Insert( red::CalculateHash64WithLength( uppercaseKey.AsChar(), uppercaseKey.Length() ), static_cast< Uint32 >( entry.m_primaryKey ) );
					}
					
					m_stringCacheOldKeys.Insert( keyHash, static_cast< Uint32 >( entry.m_primaryKey ) );
				}
				else
				{
					RED_DATA_ERROR( red::LoggerCategory_Localization, false, "Duplicate OnScreen key %hs", entry.m_secondaryKey.AsChar() );
					continue;
				}
			}
		}
		m_stringCache.Shrink();
		m_stringCacheOldKeys.Shrink();
		m_stringCacheMale.Shrink();
	}

	void OnScreensRepositoryBase::LoadAsync(const res::ResourcePath& resPath, job::Counter& counter)
	{
		if (!m_stringCache.Empty())
			m_stringCache.Clear();

		if (!m_stringCacheOldKeys.Empty())
			m_stringCacheOldKeys.Clear();

		job::Builder builder{ job::Priority::Latent };

		TResAsyncRef< JsonResource > asyncRefJson(resPath);

		if (asyncRefJson.Empty()) 
		{
			return;
		}

		res::ResourceTokenHandle token = asyncRefJson.IssueLoadingRequest();
		builder.DispatchWait(token->GetWaitCounter());
		builder.DispatchJob("audioMetadataManager/onEventMetadataLoaded", [this, token](const job::RunContext&)
		{
			if (token->IsLoaded())
			{
				const THandle< JsonResource > resJson = SafeCast< JsonResource >(token->GetResource());
				THandle< localizationPersistence::OnScreenEntries > onScreensEntryArray = resJson->GetRoot< localizationPersistence::OnScreenEntries >();
				for (const OnScreenEntry& entry : onScreensEntryArray->m_entries)
				{
					Uint64 dbKey = entry.m_primaryKey;
					
					red::DynArray< red::String > variantsArray{ ::localizationPersistence::PoolLocLocalizedText() };

					if (entry.m_maleVariant.Length() > 0)
					{
						variantsArray = red::DynArray< red::String >{ {entry.m_femaleVariant, entry.m_maleVariant }, { ::localizationPersistence::PoolLocLocalizedText() } };
					}
					else if (entry.m_femaleVariant.Length() > 0)
					{
						variantsArray = red::DynArray< red::String >{ { entry.m_femaleVariant }, { ::localizationPersistence::PoolLocLocalizedText() } };
					}

					if (!m_stringCache.KeyExist(dbKey))
					{
						if (!variantsArray.Empty())
						{
							m_stringCache.Insert(dbKey, entry.m_femaleVariant );
						}
					}
					else
					{
						RED_DATA_ERROR(red::LoggerCategory_Localization, false, "Duplicate OnScreen key %d", dbKey);
						continue;
					}

					if (entry.m_secondaryKey.Length() > 0)
					{
						Uint64 keyHash = 0;
						String uppercaseKey = entry.m_secondaryKey.AsChar();
						uppercaseKey.ToUpper();
						keyHash = red::CalculateHash64WithLength( uppercaseKey.AsChar(), uppercaseKey.Length() );

						if (!m_stringCacheOldKeys.KeyExist(keyHash))
						{
							if (!variantsArray.Empty())
							{
								m_stringCacheOldKeys.Insert( keyHash, static_cast< Uint32 >( entry.m_primaryKey ) );
							}
						}
						else
						{
							RED_DATA_ERROR(red::LoggerCategory_Localization, false, "Duplicate OnScreen key %hs", entry.m_secondaryKey.AsChar());
							continue;
						}
					}
				}
			}
		});

		counter += builder.ExtractWaitCounter();
	}
}
