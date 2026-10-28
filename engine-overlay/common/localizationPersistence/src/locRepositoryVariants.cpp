#include <cstdio>
#include "build.h"

#include "../include/locRepositoryVariants.h"
#include "../../../common/redReflection/include/gatheredResource.h"
#include "../../../common/resource/include/jsonResource.h"

#include <algorithm>

RTTI_BEGIN_TYPE_IN_NAMESPACE( SubtitleMapEntry, localizationPersistence );
RTTI_PROPERTY( m_subtitleGroup ).readOnly();
RTTI_PROPERTY( m_subtitleFile ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( SubtitleMap, localizationPersistence );
RTTI_PARENT_TYPE( ISerializable );
RTTI_PROPERTY( m_entries ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( SubtitleEntry, localizationPersistence );
RTTI_PROPERTY( m_stringId ).readOnly();
RTTI_PROPERTY( m_femaleVariant ).readOnly();
RTTI_PROPERTY( m_maleVariant ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( SubtitleEntries, localizationPersistence );
RTTI_PARENT_TYPE( ISerializable );
RTTI_PROPERTY( m_entries ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( OnScreenEntry, localizationPersistence );
RTTI_PROPERTY( m_primaryKey ).readOnly();
RTTI_PROPERTY( m_secondaryKey ).readOnly();
RTTI_PROPERTY( m_femaleVariant ).readOnly();
RTTI_PROPERTY( m_maleVariant ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( OnScreenEntries, localizationPersistence );
RTTI_PARENT_TYPE( ISerializable );
RTTI_PROPERTY( m_entries ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( LocDataMapEntry, localizationPersistence );
RTTI_PROPERTY( m_langCode ).readOnly();
RTTI_PROPERTY( m_onscreensPath ).readOnly();
RTTI_PROPERTY( m_subtitlePath ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( LocDataMap, localizationPersistence );
RTTI_PARENT_TYPE( ISerializable );
RTTI_PROPERTY( m_entries ).readOnly();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE(CLNumberDateContainer, localizationPersistence);
RTTI_PARENT_TYPE(ISerializable);
RTTI_PROPERTY(m_clNumber).readOnly();
RTTI_PROPERTY(m_clTimestamp).readOnly();
RTTI_END_TYPE();

namespace localizationPersistence
{
	res::GatheredResourceReference g_locdatamapResource( RED_CONST_RESOURCEPATH( "base\\localization\\locdatamap.json" ), GRF_Default );

	THandle< localizationPersistence::LocDataMap > SyncLoadLocMap( red::HashMap< CName, res::ResourcePath >& subtitleMap, red::HashMap< CName, res::ResourcePath >& onscreensMap )
	{
		THandle< localizationPersistence::LocDataMap > dataMap;

		res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( g_locdatamapResource.GetPath() );
		token->WaitUntilLoaded();
        std::fprintf(stderr,"CP2077 localization probe: locdatamapLoaded=%u\n",unsigned(token->IsLoaded()));

		if ( token->IsLoaded() )
		{
			const THandle< JsonResource > resJson = SafeCast< JsonResource >( token->GetResource() );
			dataMap = resJson->GetRoot< localizationPersistence::LocDataMap >();
			for ( const LocDataMapEntry& item : dataMap->m_entries )
			{
				onscreensMap.Insert( item.m_langCode, item.m_onscreensPath.GetPath() );
				subtitleMap.Insert( item.m_langCode, item.m_subtitlePath.GetPath() );
			}
		}

		return dataMap;
	}

	THandle< localizationPersistence::SubtitleMap > SyncLoadSubtitleMap( const res::ResourcePath& resPath )
	{
		THandle< localizationPersistence::SubtitleMap > dataMap;

		if ( !resPath.IsValid() )
		{
			return dataMap;
		}

		res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( resPath );
		token->WaitUntilLoaded();

		if ( token->IsLoaded() )
		{
			const THandle< JsonResource > resJson = SafeCast< JsonResource >( token->GetResource() );
			dataMap = resJson->GetRoot< localizationPersistence::SubtitleMap >();
		}

		return dataMap;
	}

	THandle< localizationPersistence::OnScreenEntries > SyncLoadLocRepositoryVariant( const res::ResourcePath& resPath )
	{
		THandle< localizationPersistence::OnScreenEntries > onscreens;
	
		if ( !resPath.IsValid() )
		{
			return onscreens;
		}
		
		res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( resPath );
		token->WaitUntilLoaded();

		if( token->IsLoaded() )
		{
			const THandle< JsonResource > resJson = SafeCast< JsonResource >( token->GetResource() );
			onscreens = resJson->GetRoot< localizationPersistence::OnScreenEntries >();
		}

		return onscreens;
	}

}

////////////////////////////////////////////////////////////////////////////
