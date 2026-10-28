/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include "appearanceResource.h"
#include "appearanceDefinition.h"
#include "appearancePart.h"
#include "appearanceCensorship.h"

#include "../../redReflection/include/cloningUtils.h"
#include "../../redReflection/include/streamedResourceManager.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../../common/redContainers/include/string/stringUtils.h"


// [nailfix] opt-out probe: report which definition of a player NAILS appearance resource is requested (CC option,
// fallback, preset or default). Nails .app files are recognised by their first definition name (..._nails_checker),
// so no path strings are needed in cooked builds. Count-limited (48 lines).
namespace cpweb_nailfix_shared
{
	// Set by gameUI characterCustomizationSystem.cpp while the CC state is the developer fallback preset.
	std::atomic< int > g_fallbackCustomization{ 0 };
}
namespace cpweb_nailfix
{
	// "..._nails_checker" -> "..._nails_beige" (the character-creation default nail colour), else empty.
	static bool BeigeFor( const char* requested, char* out, size_t outSize )
	{
		if ( !requested ) return false;
		const size_t n = std::strlen( requested );
		static const char suffix[] = "__nails_checker";
		const size_t k = sizeof( suffix ) - 1;
		if ( n < k || std::strcmp( requested + n - k, suffix ) != 0 || n - 7 + 5 + 1 > outSize ) return false;
		std::memcpy( out, requested, n - 7 );
		std::memcpy( out + n - 7, "beige", 6 );
		return true;
	}
	static bool IsNailsResource( const appearance::AppearanceResource::TAppearances& apps )
	{
		if ( apps.Empty() || !apps[ 0 ] ) return false;
		const char* first = apps[ 0 ]->GetName().AsChar();
		return first && std::strstr( first, "nails_checker" ) != nullptr;
	}
	static void Report( const char* what, const char* requested, const char* found, unsigned count )
	{
		static std::atomic< unsigned > s_reports{ 0 };
		if ( s_reports.fetch_add( 1 ) < 48 )
		{
			std::fprintf( stderr, "CP2077 nailfix probe: %s requested=%s found=%s definitions=%u\n", what, requested ? requested : "-", found ? found : "-", count );
		}
	}
}

RTTI_BEGIN_TYPE_IN_NAMESPACE( CensorshipEntry, appearance );
RTTI_PROPERTY( original ).setName( "Original" );
RTTI_PROPERTY( censored ).setName( "Censored" );
RTTI_PROPERTY( censorFlags ).setName( "CensorFlags" ); 
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( AppearanceResource, appearance );
RTTI_PARENT_TYPE( res::StreamedResource )
RTTI_PROPERTY( m_baseType ).readOnly().notCooked();
RTTI_PROPERTY( m_baseEntityType );
RTTI_PROPERTY( m_baseEntity ).notCooked();
RTTI_PROPERTY( m_partType );
RTTI_PROPERTY( m_preset );
RTTI_PROPERTY( m_appearances );
RTTI_PROPERTY( m_censorshipMapping ).readOnly();
RTTI_PROPERTY( m_commonCookData ).cookedOnly();
RTTI_PROPERTY( m_dismWounds ).setName( "Wounds" ).resizable();
RTTI_PROPERTY( m_dismEffects ).setName( "DismEffects" ).resizable();
RTTI_PROPERTY( m_dismWoundConfig ).setName( "DismWoundConfig" ).editable();
#ifndef RED_CONFIGURATION_FINAL
RTTI_PROPERTY( m_backendData ).notCooked();
#endif
RTTI_PROPERTY_CATEGORY( "Proxy Generation Settings" );
RTTI_PROPERTY( m_proxyPolyCount ).editable().range( 1000, 8000, true );
RTTI_PROPERTY( m_forceCompileProxy ).editable();
RTTI_END_TYPE();

Config::TConfigVar<String> cvAppearanceDefinitionsNameSuffixSeparator( "AppearanceDefinitions", "SuffixSeparator", "&", Config::EConfigVarFlags::eConsoleVarFlag_ReadOnly );

namespace appearance
{
	typedef THandle< AppearanceDefinition > TAppearanceDefinition;

	static const CName s_DefaultAppearanceName = RED_NAME_CONSTEXPR( "default" );

	const Int32 c_defaultProxyPolyCount = 1400;
	
	/////////////////////////////////////////////////////////////////////////////////////////////////
	AppearanceResource::AppearanceResource()
		: m_proxyPolyCount( c_defaultProxyPolyCount )
	{}

	Bool AppearanceResource::SetBaseEntityType( const CName& baseType )
	{
		if ( baseType != m_baseEntityType )
		{
			m_baseEntityType = baseType;
			return true;
		}
		return false;
	}

	Bool AppearanceResource::HasAppearance( const CName& name ) const
	{
		RED_SCOPE_SHARED_LOCK( m_appearancesLock );

		auto result = std::find_if( m_appearances.Begin(), m_appearances.End(), [name]( const TAppearanceDefinition& def ) { return def->GetName() == name; } );
		return ( result != m_appearances.End() );
	}

    const TAppearanceDefinition AppearanceResource::GetAppearance( const CName& name, Uint32 censorFlags ) const
	{
		RED_SCOPE_SHARED_LOCK( m_appearancesLock );

        CName nameToFind = name;
        if( censorFlags )
        {
            CensorshipEntry toFind;
            toFind.original = name;
            
            if( const CensorshipEntry* censorship = m_censorshipMapping.FindPtr( toFind ) )
            {
                if( censorFlags & censorship->censorFlags )
                {
                    nameToFind = censorship->censored;
                }
            }
        }

		auto result = std::find_if( m_appearances.Begin(), m_appearances.End(), [nameToFind]( const TAppearanceDefinition& def ) { return def->GetName() == nameToFind; } );

		if ( cpweb_nailfix::IsNailsResource( m_appearances ) )
		{
			// [nailfix] The developer fallback CC preset (no character creation) selects the UV-test 'checker' nails
			// (green/black checker albedo). Show the character-creation default (beige) instead; a real CC/saved state
			// clears the flag, so a deliberately chosen checker design is untouched.
			char beige[ 160 ];
			if ( cpweb_nailfix_shared::g_fallbackCustomization.load() && cpweb_nailfix::BeigeFor( nameToFind.AsChar(), beige, sizeof( beige ) ) )
			{
				const char* beigeName = beige;
				auto alternative = std::find_if( m_appearances.Begin(), m_appearances.End(), [beigeName]( const TAppearanceDefinition& def ) { const char* n = def ? def->GetName().AsChar() : nullptr; return n && std::strcmp( n, beigeName ) == 0; } );
				if ( alternative != m_appearances.End() )
				{
					cpweb_nailfix::Report( "fallbackRemap", nameToFind.AsChar(), ( *alternative )->GetName().AsChar(), Uint32( m_appearances.Size() ) );
					return *alternative;
				}
			}
			cpweb_nailfix::Report( "byName", nameToFind.AsChar(), result != m_appearances.End() ? ( *result )->GetName().AsChar() : nullptr, Uint32( m_appearances.Size() ) );
		}

		if( result != m_appearances.End() )
		{
			return *result;
		}
		return nullptr;
	}

	const THandle< AppearanceDefinition > AppearanceResource::GetAppearance( Uint32 index ) const
	{
		RED_SCOPE_SHARED_LOCK( m_appearancesLock );
		if ( cpweb_nailfix::IsNailsResource( m_appearances ) )
		{
			char idx[ 16 ];
			std::snprintf( idx, sizeof( idx ), "#%u", index );
			cpweb_nailfix::Report( "byIndex", idx, index < m_appearances.Size() && m_appearances[ index ] ? m_appearances[ index ]->GetName().AsChar() : nullptr, Uint32( m_appearances.Size() ) );
		}
		return m_appearances[ index ];
	}

	const AppearanceResource::TAppearances AppearanceResource::GetAppearancesWithTags( const red::TagList& tagList ) const
	{
		RED_SCOPE_SHARED_LOCK( m_appearancesLock );

		AppearanceResource::TAppearances result{ red::PoolEngine() };
		result.Reserve( m_appearances.Size() );

		for ( const TAppearanceDefinition& app : m_appearances )
		{
			// now it's gonna check if app has all tags from tagList
			if ( red::TagList::MatchAll( tagList, app->GetVisualTags() ) )
			{
				result.EmplaceBack( app );
			}
		}

		return result;
	}

	Bool AppearanceResource::AddAppearance( const CName& name )
	{
		RED_SCOPE_LOCK( m_appearancesLock );
		return AddAppearance_NoLock( name );
	}

	Bool AppearanceResource::AddAppearance( const AppearanceDefinition& apperanceDefinition )
	{
		RED_SCOPE_LOCK( m_appearancesLock );
		return AddAppearance_NoLock( apperanceDefinition );
	}

	Bool AppearanceResource::AddAppearance_NoLock( const CName& name )
	{
		const String nameString( name.AsChar() );
		auto result = std::find_if( m_appearances.Begin(), m_appearances.End(), [nameString]( const TAppearanceDefinition& def ) { return nameString.EqualsNC( def->GetName().AsChar() ); } );

		if( result != m_appearances.End() )
		{
			return false;
		}

		m_appearances.PushBack( CreateHandle< AppearanceDefinition >( name ) );
		return true;
	}

	Bool AppearanceResource::AddAppearance_NoLock( const AppearanceDefinition& appearanceDefinition )
	{
		const String nameString( appearanceDefinition.GetName().AsChar() );
		if( std::find_if( m_appearances.Begin(), m_appearances.End(), [nameString]( const TAppearanceDefinition& def ) { return nameString.EqualsNC( def->GetName().AsChar() ); } ) != m_appearances.End() )
		{
			return false;
		}

        // Clone the appearance using RTTI.
		// This is because of DeferredDataBuffer used in the CharacterAppearanceDefinition prevents use of regular copy constructor.
		THandle< AppearanceDefinition > appearance = appearanceDefinition.HandleFromThis< AppearanceDefinition >();
		RED_FATAL_ASSERT( appearance != nullptr, "Cannot make a copy of character appearance that is not owned by a THandle" );

		THandle< AppearanceDefinition > appearanceCopy = Cast< AppearanceDefinition >( tools::CloneObject( appearance ) );

		m_appearances.PushBack( appearanceCopy );
		return true;
	}
	
    Bool AppearanceResource::AddCensoredAppearance_NoLock( const CName& nameToCensor, const AppearanceDefinition& censoredApperanceDefinition )
    {
        String nameToCensorString( nameToCensor.AsChar() );
        nameToCensorString.ToLower();

        auto found = std::find_if( m_appearances.Begin(), m_appearances.End(),
            [nameToCensorString]( const TAppearanceDefinition& def ) { return nameToCensorString.EqualsNC( def->GetName().AsChar() ); } );

        if( found == m_appearances.End() )
            return false;

        Bool result = false;

        RemoveAppearance_NoLock( censoredApperanceDefinition.GetName() );
        if( AddAppearance_NoLock( censoredApperanceDefinition ) )
        {
            CensorshipEntry entry;
            entry.original = nameToCensor;
            entry.censored = censoredApperanceDefinition.GetName();
            entry.censorFlags = censoredApperanceDefinition.GetCensorFlags();

            red::alg::PushBackUnique( m_censorshipMapping, entry );
            result = true;
        }

        NotifyPropertyChanged( rtti::AccessPath( "appearances" ) );
        NotifyPropertyChanged( rtti::AccessPath( "censorshipMapping" ) );

        return result;
    }

	void AppearanceResource::RefreshConsorshipMap()
	{
		m_censorshipMapping.Clear();

		for ( THandle< AppearanceDefinition > & definition : m_appearances )
		{
			if ( definition->IsCensorshipAppearance() )
			{				
				CensorshipEntry entry;
				entry.original = definition->GetParentAppearanceName();
				entry.censored = definition->GetName();
				entry.censorFlags = definition->GetCensorFlags();

				red::alg::PushBackUnique( m_censorshipMapping, entry );
			}				
		}

		NotifyPropertyChanged( rtti::AccessPath( "censorshipMapping" ) );
	}

    Bool AppearanceResource::AddCensoredAppearance( const CName& nameToCensor, const AppearanceDefinition& censoredApperanceDefinition )
    {
		RED_SCOPE_LOCK( m_appearancesLock );
        return AddCensoredAppearance_NoLock( nameToCensor, censoredApperanceDefinition );
    }

	Bool AppearanceResource::RemoveAppearance( const CName& name )
	{
		RED_SCOPE_LOCK( m_appearancesLock );
		return RemoveAppearance_NoLock( name );
	}
	Bool AppearanceResource::RenameAppearance( const CName& oldName, const CName& newName )
	{
		RED_SCOPE_LOCK( m_appearancesLock );
		return RenameAppearance_NoLock( oldName, newName );
	}

	THandle<appearance::AppearanceDefinition> AppearanceResource::CloneAppearance( const CName& name )
	{
		RED_SCOPE_LOCK( m_appearancesLock );

		auto result = std::find_if( m_appearances.Begin(), m_appearances.End(),
			[name]( const TAppearanceDefinition& def ) { return name == def->GetName(); } );

		if( result != m_appearances.End() )
		{
			return CloneAppearance_NoLock( *result );
		}
		return nullptr;
	}

	void AppearanceResource::SetInheritedVisualTags( const CName& name, const red::TagList& tags )
	{
		RED_SCOPE_LOCK( m_appearancesLock );
		auto result = std::find_if( m_appearances.Begin(), m_appearances.End(),
									[name]( const TAppearanceDefinition& def ) { return name == def->GetName(); } );

		if( result != m_appearances.End() )
		{
			(*result)->SetInheritedVisualTags( tags );
		}
	}

	Bool AppearanceResource::RemoveAppearance_NoLock( const CName& name )
	{
		auto result = std::find_if( m_appearances.Begin(), m_appearances.End(), [name]( const TAppearanceDefinition& def ) { return def->GetName() == name; } );
        if( result != m_appearances.End() )
        {
			CName orginalName;
			if ( result->Get()->IsCensorshipAppearance() )
			{
				orginalName = result->Get()->GetParentAppearanceName();
			}

            if( m_appearances.Remove( result ).IsSuccessful() )
            {
				if ( !orginalName.Empty() )
				{
					CensorshipEntry toRemove;
					toRemove.original = orginalName;
					m_censorshipMapping.Remove( toRemove );

					NotifyPropertyChanged( rtti::AccessPath( "censorshipMapping" ) );
				}

                return true;
            }
        }
        return false;
	}

	Bool AppearanceResource::RenameAppearance_NoLock( const CName& oldName, const CName& newName )
	{
		String newNameString( newName.AsChar() );
		newNameString.ToLower();

		auto result = std::find_if( m_appearances.Begin(), m_appearances.End(),
									[newNameString]( const TAppearanceDefinition& def ) { return newNameString.EqualsNC( def->GetName().AsChar() ); } );

		if( result != m_appearances.End() )
		{
			return false;
		}

		auto item = std::find_if( m_appearances.Begin(), m_appearances.End(),
								  [oldName]( const TAppearanceDefinition& def ) { return def->GetName() == oldName; } );

		if( item == m_appearances.End() )
		{
			return false;
		}

		( *item )->SetName( newName );

		return true;
	}

	THandle< appearance::AppearanceDefinition > AppearanceResource::CloneAppearance_NoLock( const THandle< AppearanceDefinition >& appearanceDefinition )
	{
		return Cast< AppearanceDefinition >( tools::CloneObject( appearanceDefinition ) );
	}

	ent::dismemberment::WoundsConfigSet& AppearanceResource::GetDismembermentWoundConfigSet()
	{
		return m_dismWoundConfig;
	}

	const ent::dismemberment::WoundsConfigSet& AppearanceResource::GetDismembermentWoundConfigSet() const
	{
		return m_dismWoundConfig;
	}

	void AppearanceResource::OnPostLoad( const PostLoadContext& context )
	{
		TBaseClass::OnPostLoad( context );

		if( m_appearances.Empty() )
		{
			m_appearances.PushBack( CreateHandle< AppearanceDefinition >( s_DefaultAppearanceName ) );
		}
	}

	void AppearanceResource::GetCustomEditableProperties( rtti::EditableProperties& outRootProperties ) const
	{
		// BaseType -> weapon, weapon part, chacter etc.
		// Base Entity Type -> base body type (man avrg, handgun etc)
		// Part Type -> Only for weapon parts: part type name: magazine, reciver, etc.
		// Base Entity -> base entity of appearance ( reference)
		// preset -> weapon perset

		rtti::ClassEditablePropertyInfo info;
		info.m_name = RED_NAME_CONSTEXPR( "baseEntityType" );
		info.m_type = GetTypeObject< CName >();

		if ( IsWeaponAppearance() )
		{
			info.m_canReset = false;
			info.m_isReadOnly = true;
		}
		else if ( IsWeaponPartAppearance() )
		{
			info.m_customTypeName = RED_NAME_CONSTEXPR( "appearanceBaseEntityType" );			
		}
		else if ( IsCustom( m_baseType ) || IsCorpse( m_baseType ) )
		{
			info.m_name = RED_NAME_CONSTEXPR( "baseEntity" );
			info.m_type = GetTypeObject< TResRef< ent::EntityTemplate > >();
			info.m_isReadOnly = IsCorpse( m_baseType );
		}
		else if ( m_baseType != CName::NONE() )
		{
			info.m_customTypeName = RED_NAME_CONSTEXPR( "appearanceBaseEntity" );
		}

		outRootProperties.Add( info );

		if ( IsWeaponPartAppearance() )
		{
			{
				rtti::ClassEditablePropertyInfo info;
				info.m_name = RED_NAME_CONSTEXPR( "partType" );
				info.m_type = GetTypeObject< CName >();
				info.m_customTypeName = RED_NAME_CONSTEXPR( "appearancePartType" );

				outRootProperties.Add( info );
			}
		}

		if ( IsWeaponAppearance() )
		{
			rtti::ClassEditablePropertyInfo info;
			info.m_name = RED_NAME_CONSTEXPR( "preset" );
			info.m_type = GetTypeObject< CName >();
			info.m_isReadOnly = m_baseType == RED_NAME_CONSTEXPR_NOREG( "preset" );
			info.m_customTypeName = RED_NAME_CONSTEXPR( "TweakDBAppearancePreset" );

			outRootProperties.Add( info );
		}

		CResource::GetCustomEditableProperties( outRootProperties );
	}

	Bool AppearanceResource::ReadCustomEditableProperty( IRTTIContext& ctx, const CName propertyName, const rtti::AccessPath& restOfThePath, red::SharedPtr< rtti::ValueHolder >& outValue ) const
	{
		if( restOfThePath.IsEmpty() )
		{
			if ( IsWeaponPartAppearance() )
			{
				if ( propertyName == RED_NAME_CONSTEXPR_NOREG( "partType" ) )
				{
					outValue = rtti::ValueHolder::CreateSingle( m_partType.AsChar() );
					return true;
				}
			}
			else if ( IsWeaponAppearance() )
			{
				if ( propertyName == RED_NAME_CONSTEXPR_NOREG( "preset" ) )
				{
					outValue = rtti::ValueHolder::Parse( m_preset.AsChar() );
					return true;
				}
			}
		}
		return CResource::ReadCustomEditableProperty( ctx, propertyName, restOfThePath, outValue );
	}

	Bool AppearanceResource::WriteCustomEditableProperty( IRTTIContext& ctx, const CName propertyName, const rtti::AccessPath& restOfThePath, const red::SharedPtr< rtti::ValueHolder >& newValue )
	{
		if( restOfThePath.IsEmpty() )
		{
			if ( IsWeaponPartAppearance() )
			{
				if ( propertyName == RED_NAME_CONSTEXPR_NOREG( "partType" ) )
				{
					m_partType = RED_NAME( newValue->GetSingleValue() );
					NotifyPropertyChanged( rtti::AccessPath( propertyName.AsChar() ) );
					return true;
				}
			}
			else if ( IsWeaponAppearance() )
			{
				if ( propertyName == RED_NAME_CONSTEXPR_NOREG( "preset" ) )
				{
					m_preset = RED_NAME( newValue->ToString() );
					NotifyPropertyChanged( rtti::AccessPath( propertyName.AsChar() ) );
					return true;
				}
			}
		}
		return CResource::WriteCustomEditableProperty( ctx, propertyName, restOfThePath, newValue );
	}

	namespace dismemberment
	{
		template< typename Tlist >
		Uint32 FindItem( const Tlist& list, const CName& name )
		{
			auto it = std::find_if( list.Begin(), list.End(), [name]( const typename Tlist::value_type& element ) { return element->name == name; } );
			return ( it != list.End() ) ? ( Uint32 )( it - list.Begin() ) : UINT32_MAX;
		}

		template< typename Titem, typename Tlist >
		Uint32 AddItem( Tlist& list, const CName& name )
		{
			Uint32 index = FindItem( list, name );
			if( index == UINT32_MAX )
			{
				index = list.Size();
				auto item = CreateHandle< Titem >( name );
				list.PushBack( item );
			}
			return index;
		}

		template< typename Tlist >
		Bool RemoveItem( Tlist& list, const CName& name )
		{
			Uint32 index = FindItem( list, name );
			if( index != UINT32_MAX )
			{
				list.RemoveAt( index );
			}
			return ( index != UINT32_MAX );
		}
	} //

	Uint32 AppearanceResource::AddDismembermentWound( const CName& name )
	{
		const Uint32 index = dismemberment::AddItem< ent::dismemberment::WoundResource >( m_dismWounds, name );
		if( index != UINT32_MAX )
		{
			THandle< ent::dismemberment::WoundResource > wound = m_dismWounds[ index ];
			wound->cullObject.capA = Vector3::EZ() * 0.1f;
			wound->cullObject.capB = Vector3::ZEROS();
		}
		return index;
	}

	Bool AppearanceResource::RemoveDismembermentWound( const CName& name )
	{
		return dismemberment::RemoveItem( m_dismWounds, name );
	}

	Uint32 AppearanceResource::FindDismembermentWound( const CName& name )
	{
		return dismemberment::FindItem( m_dismWounds, name );
	}
	
	Uint32 AppearanceResource::AddDismembermentEffect( const CName& name )
	{
		return dismemberment::AddItem< ent::dismemberment::EffectResource >( m_dismEffects, name );
	}

	Bool AppearanceResource::RemoveDismembermentEffect( const CName& name )
	{
		return dismemberment::RemoveItem( m_dismEffects, name );
	}

	Uint32 AppearanceResource::FindDismembermentEffect( const CName& name )
	{
		return dismemberment::FindItem( m_dismEffects, name );
	}

#ifndef RED_CONFIGURATION_FINAL
	const THandle< IBackendData >& AppearanceResource::GetBackendData() const
	{
		return m_backendData;
	}
	void AppearanceResource::SetBackendData( const THandle< IBackendData >& data )
	{
		m_backendData = data;
	}
#endif
	Bool AppearanceResource::IsWeaponAppearance() const
	{
		return m_baseType == RED_NAME_CONSTEXPR_NOREG( "Weapon (NPC)" );
	}

	Bool AppearanceResource::IsWeaponPartAppearance() const
	{
		return m_baseType == RED_NAME_CONSTEXPR_NOREG( "Weapon Part (Player Custom)" );
	}

	CName AppearanceResource::GenerateWeaponPartAppearanceName( const CName weaponPreset, const CName weaponAppearanceDefinition )
	{
		red::String result = red::StrCat( weaponPreset.AsStringView(), { ":" }, weaponAppearanceDefinition.AsStringView() );
		return RED_NAME( result );
	}

	void AppearanceResource::ComputeStreamingData( res::StreamingData& result, CName appearanceName ) const
	{
		RED_SCOPE_SHARED_LOCK( m_appearancesLock );

		result.m_streamingDistance = 10.0f;
		res::StreamingResourceManager * streamingSystem = GetStreamingResourceManager();
		
		for( const auto & definition : m_appearances )
		{
			const bool appearanceFound = definition->GetName() == appearanceName;

			if( appearanceFound || appearanceName.Empty() )
			{
				for( const auto& part : definition->GetParts() )
				{
					const res::StreamingData data = streamingSystem->GetStreamingData(part.GetPath());
					result.m_streamingDistance = std::max(result.m_streamingDistance, data.m_streamingDistance);
					result.m_boundingBox.AddBox( data.m_boundingBox );
					result.m_visibleBoundingBox.AddBox( data.m_visibleBoundingBox );
				}

				if ( !IsCorpse( m_baseType ) )
				{
					result.m_proxyHash = definition->GetProxyMeshPath().GetHash();
				}

				if( appearanceFound )
					break;
			}
			 
		}
	}	

	void AppearanceResource::GetAllResourceDependencies( red::DynArray<res::ResourcePath>& result ) const
	{
		RED_SCOPE_SHARED_LOCK( m_appearancesLock );

		for( const auto & definition : m_appearances )
		{
			for( const auto & part : definition->GetParts() )
			{
				const res::ResourcePath path = part.GetPath();

				if( path.IsValid() )
				{
					result.PushBack( path );
				}
			}
		}
	}

	void AppearanceResource::ScheduleCookingJobs( job::Builder& jobBuilder, ECookingPlatform cookingPlatform )
	{
		job::Counter counter;
		auto deferral = counter.CreateDeferral();
		jobBuilder.DispatchWait( counter );
		{
			THandle< AppearanceResource > thisHandle = HandleFromThis<AppearanceResource>();
			RED_SCOPE_LOCK( m_appearancesLock );

			{ // remove all censorship appearances to avoid zombies
				red::DynArray<CName> names{ red::PoolBackend() };
				for( THandle< AppearanceDefinition > & definition : m_appearances )
				{
					if( definition->IsCensorshipAppearance() && definition->GetName().AsStringView().EndsWith( "_censored" ) )
					{
						names.PushBack( definition->GetName() );
					}
				}

				for( const CName& name : names )
				{
					RemoveAppearance_NoLock( name );
				}
				m_censorshipMapping.Clear();
			}

			{
				/*
				Because of deferred buffers (compiled data, params) and cloning don't like each others,
				we need to first clone and add censored appearances and then compile
				(adding an appearanceDefinition requires cloning^ and after that in cloned appearance deferred buffers are not loaded
				and may not be saved correctly).

				^ copying appearances via copy constructor or assignment operator is not allowed because of existence of deferred buffers.
				*/
				red::DynArray<CName> originalNames{ red::PoolBackend() };
				red::DynArray<THandle<AppearanceDefinition>> censoredDefinitions{ red::PoolBackend() };
				for( THandle< AppearanceDefinition > & definition : m_appearances )
				{
					const Uint32 appearanceCensorFlags = appearance::ResolveCensorshipFlags( definition->GetVisualTags().GetTagNames() );
					if( appearanceCensorFlags && !definition->IsCensorshipAppearance() )
					{
						THandle<appearance::AppearanceDefinition> censoredDefinition = CloneAppearance_NoLock( definition );
						censoredDefinition->MarkAsCensored( appearanceCensorFlags, thisHandle );
						censoredDefinitions.EmplaceBack( std::move( censoredDefinition ) );
						
						originalNames.PushBack( definition->GetName() );
					}
				}

				for( Uint32 i = 0; i < censoredDefinitions.Size(); ++i )
				{
					THandle< AppearanceDefinition > & definition = censoredDefinitions[i];
					AddCensoredAppearance_NoLock( originalNames[i], *definition );
				}
			}

			for( THandle< AppearanceDefinition > & definition : m_appearances )
			{
				definition->ScheduleAppearanceCookingJobs( jobBuilder, thisHandle, m_baseEntity.GetPath(), GetPath(), m_baseType, cookingPlatform );
			}
		}

		deferral.FinishDeferral();
	}

	void AppearanceResource::Cooker_LoadEnttiyParameterBuffers( job::Builder& builder )
	{
		for( THandle< AppearanceDefinition > & definition : m_appearances )
		{
			definition->Cooker_LoadEnttiyParameterBuffers( builder );
		}
	}

	bool AppearanceResource::ShouldAppearanceDefinitionsBeCooked() const
	{
        return IsVehicle(m_baseType) || IsCharacter( m_baseType );
	}

	res::ResourcePath AppearanceResource::ComputeCookedDataPath( const CName baseType, const res::ResourcePath& path)
	{
		const char* c_cookedAppearanceFormat = "base\\cookedAppearances\\common_%s_%016llX.cookedapp";
		char filename[512] = {};
		red::SNPrintFUnsafe( filename, sizeof(filename), c_cookedAppearanceFormat, baseType.AsChar(), path.GetHash() );
		return res::ResourcePath::Build(filename);
	}

    res::ResourcePath AppearanceResource::ComputeDefinitionCookedDataPath( const CName baseType, const res::ResourcePath& appearanceResourcePath, const CName& definitionName )
    {
        char filename[512] = {};
        red::SNPrintFSafe( filename, sizeof( filename ), "base\\cookedAppearances\\%s_%016llX_%s.cookedapp", 
            baseType.AsChar(), 
            appearanceResourcePath.GetHash(),
            definitionName.AsChar() );

        return res::ResourcePath::Build( filename );
    }

    void AppearanceResource::SetCommonAppearanceCookData( const res::ResourcePath& path )
	{
		m_commonCookData = path;
	}

	const TResAsyncRef< CookedAppearanceData >& AppearanceResource::GetCommonCookData() const
	{
		return m_commonCookData;
	}

}
