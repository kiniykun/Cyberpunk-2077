/**
* Copyright (C)2017 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <cstdio>
#include "inputKeyIconManager.h"
#include "inputKeyData.h"
#include "widgetTextureAtlas.h"

#include "../../resource/include/jsonResource.h"
#include "../../redReflection/include/gatheredResource.h"
#include "inkSystem.h"
#include "inkResourceManager.h"

RTTI_BEGIN_TYPE_IN_NAMESPACE( InputKeyIconManager, ink );
	RTTI_PARENT_TYPE(ISerializable);
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( InputIconMappingJSON, ink );
	RTTI_PROPERTY( m_id );
	RTTI_PROPERTY( m_part );
	RTTI_PROPERTY( m_hold );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( InputDevicesMappingsJSON, ink );
	RTTI_PROPERTY( m_devices );
	RTTI_PROPERTY( m_mappings );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( InputKeyDictionaryJSON, ink );
	RTTI_PARENT_TYPE( ISerializable );
	RTTI_PROPERTY( m_devicesMappings );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( InputDeviceCallback, ink );
	RTTI_PARENT_TYPE( ink::CallbackBase );
RTTI_END_TYPE();

namespace
{
	res::TStaticResource< ink::TextureAtlas > fallbackIconsAtlas( RED_CONST_RESOURCEPATH( "engine\\ink\\input\\icons_fallback.inkatlas" ), GRF_Fallback | GRF_SkipOnHeadless | GRF_Startup );

#if defined RED_PLATFORM_WINPC
	res::TStaticResource< ink::TextureAtlas > keyboardIconsAtlas( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\common\\input\\icons_keyboard.inkatlas" ), GRF_SkipOnHeadless | GRF_Startup );
	res::TStaticResource< ink::TextureAtlas > steamIconsAtlas( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\common\\input\\icons_steam.inkatlas" ), GRF_SkipOnHeadless | GRF_Startup );
#endif

#if defined RED_PLATFORM_WEB
	// [npcblack] Web build: keyboard/mouse is the only input device; register the PC keyboard icon atlas for it.
	res::TStaticResource< ink::TextureAtlas > keyboardIconsAtlas( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\common\\input\\icons_keyboard.inkatlas" ), GRF_SkipOnHeadless | GRF_Startup );
#endif

#if defined RED_PLATFORM_DURANGO || defined RED_PLATFORM_WINPC
	res::TStaticResource< ink::TextureAtlas > durangoIconsAtlas( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\common\\input\\icons_durango.inkatlas" ), GRF_SkipOnHeadless | GRF_Startup );
#endif

#if defined RED_PLATFORM_ORBIS || defined RED_PLATFORM_WINPC
	res::TStaticResource< ink::TextureAtlas > orbisIconsAtlas( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\common\\input\\icons_orbis.inkatlas" ), GRF_SkipOnHeadless | GRF_Startup );
#endif

#if defined RED_PLATFORM_WINPC
	constexpr input::EInputDevice c_initActiveInputDevice = input::EInputDevice::KBD_MOUSE;
#elif defined RED_PLATFORM_DURANGO
	constexpr input::EInputDevice c_initActiveInputDevice = input::EInputDevice::DURANGO;
#elif defined RED_PLATFORM_ORBIS
	constexpr input::EInputDevice c_initActiveInputDevice = input::EInputDevice::ORBIS;
#elif defined RED_PLATFORM_WEB
	constexpr input::EInputDevice c_initActiveInputDevice = input::EInputDevice::KBD_MOUSE; // [npcblack]
#else
	constexpr input::EInputDevice c_initActiveInputDevice = input::EInputDevice::INVALID;
#endif
}

namespace ink
{
	/* static */ const String InputKeyIconManager::s_invalidInputNameStr = String("INVALID INPUT");
	static const String s_holdInputNamePrefix("Hold ");
	const String s_keyPrefix("IK_");
	static const String s_keyPadPrefix("IK_Pad_");

	res::GatheredResourceReference g_inputKeyIconDictionaryResource( RED_CONST_RESOURCEPATH( "engine\\ink\\input\\input_key_icon_dictionary.json" ), GRF_Default );

	InputKeyIconManager::InputKeyIconManager()
		: m_activeInputDevice( c_initActiveInputDevice )
		, m_inputDeviceTextureDictionary( ink::PoolInk_System() )
		, m_inputKeyIconDictionary( g_inputKeyIconDictionaryResource.GetPath() )
		, m_jsonHandle( nullptr )
		, m_keyStringToIdMap( ink::PoolInk_System() )
		, m_keyIdToStringMap( ink::PoolInk_System() )
	{
		FillKeyNamesMap();
		LoadSupportedInputDevices();
		LoadTextureDictionaryJSON();
	}

	void InputKeyIconManager::FillKeyNamesMap()
	{
		CustomInputKeys::FillCombinedConversionMap( m_keyStringToIdMap );

		TKeyIntToString keyIntToName{ ink::PoolInk_System() };
		CustomInputKeys::FillCombinedConversionMap( keyIntToName );

		// Remove prefix to make UI-friendly names
		for (TKeyIntToString::iterator it = keyIntToName.Begin(); it != keyIntToName.End(); ++it)
		{
			String str = it.Value();

			if( str.BeginsWith( s_keyPadPrefix ) )
			{
				str = str.StringAfter( s_keyPadPrefix );
			}
			else
			{
				str = str.StringAfter( s_keyPrefix );
			}

			m_keyIdToStringMap.Insert( it.Key(), str );
		}
	}

	void InputKeyIconManager::LoadSupportedInputDevices()
	{
#if defined RED_PLATFORM_WINPC
		AddSupportedInputDevice( input::EInputDevice::KBD_MOUSE, keyboardIconsAtlas.IsLoaded() ? keyboardIconsAtlas.Get() : fallbackIconsAtlas.Get() );
		AddSupportedInputDevice( input::EInputDevice::DURANGO, durangoIconsAtlas.IsLoaded() ? durangoIconsAtlas.Get() : fallbackIconsAtlas.Get() );
		AddSupportedInputDevice( input::EInputDevice::XINPUT_PAD, durangoIconsAtlas.IsLoaded() ? durangoIconsAtlas.Get() : fallbackIconsAtlas.Get() );
		AddSupportedInputDevice( input::EInputDevice::ORBIS, orbisIconsAtlas.IsLoaded() ? orbisIconsAtlas.Get() : fallbackIconsAtlas.Get() );
		AddSupportedInputDevice( input::EInputDevice::STEAM, steamIconsAtlas.IsLoaded() ? steamIconsAtlas.Get() : fallbackIconsAtlas.Get() );
#elif defined RED_PLATFORM_DURANGO
		AddSupportedInputDevice( input::EInputDevice::DURANGO, durangoIconsAtlas.IsLoaded() ? durangoIconsAtlas.Get() : fallbackIconsAtlas.Get() );
#elif defined RED_PLATFORM_ORBIS
		AddSupportedInputDevice( input::EInputDevice::ORBIS, orbisIconsAtlas.IsLoaded() ? orbisIconsAtlas.Get() : fallbackIconsAtlas.Get() );
#elif defined RED_PLATFORM_WEB
		AddSupportedInputDevice( input::EInputDevice::KBD_MOUSE, keyboardIconsAtlas.IsLoaded() ? keyboardIconsAtlas.Get() : fallbackIconsAtlas.Get() );
		std::fprintf( stderr, "CP2077 npcblack input icons: web KBD_MOUSE device registered keyboardAtlas=%d fallbackAtlas=%d\n", int( keyboardIconsAtlas.IsLoaded() ), int( fallbackIconsAtlas.IsLoaded() ) );
#endif
	}

	void InputKeyIconManager::AddSupportedInputDevice( input::EInputDevice inputDevice, const THandle< TextureAtlas >& iconsAtlas )
	{
		RED_ASSERT( !m_inputDeviceTextureDictionary.KeyExist(inputDevice), "[InputKeyIconManager::AddSupportedInputDevice] Adding duplicate device" );
		RED_ASSERT( iconsAtlas, "[InputKeyIconManager::AddSupportedInputDevice] Icons Atlas is invalid" );

		ResourceManager &resourceManager = GetInkSystem().GetResourceManager();
		resourceManager.AddPermanentAtlas( iconsAtlas );

		InputDeviceTextureData textureData;
		textureData.m_atlas = TResRef< TextureAtlas >( iconsAtlas->GetPath() );
		m_inputDeviceTextureDictionary[ inputDevice ] = textureData;
	}

	void InputKeyIconManager::LoadInputDeviceIconMappings( const InputIconMappingJSON& inpuIconMapping, InputDeviceTextureData& textureData )
	{
		Int32 inputKeyId = GetInputKeyId( inpuIconMapping.m_id.AsChar() );
		if ( IsValidInputKey( inputKeyId ) )
		{
			if ( inpuIconMapping.m_hold )
			{
				inputKeyId = InputKeyData::RawToActualKeyID( inputKeyId );
			}

			RED_ERROR(!textureData.m_iconPartData.KeyExist(inputKeyId), "[InputIconTextureDictionary] Duplicate Input Key Id '%hs'", inpuIconMapping.m_id.AsChar());
			InputIconPartData& iconData = textureData.m_iconPartData[inputKeyId];
			iconData.m_part = inpuIconMapping.m_part;
		}
	}

	Bool InputKeyIconManager::GetInputKeyIconTextureData( const InputKeyData& inputKey, InputIconTextureData& outData, input::EInputDevice inputDevice ) const
	{
		if( IsValidInputKey( inputKey ) )
		{
			Int32 inputKeyId = inputKey.GetActualInputKeyID();

#if defined RED_PLATFORM_WINPC
			Bool isHold = false;
#endif
			if ( inputKey.IsHoldInput() )
			{
				inputKeyId = InputKeyData::RawToActualKeyID(inputKeyId);
#if defined RED_PLATFORM_WINPC
				isHold = true;
#endif
			}

			if ( inputDevice == input::EInputDevice::INVALID )
			{
				inputDevice = GetActiveInputDevice();
			}
			if( inputDevice != input::EInputDevice::INVALID )
			{
				const InputDeviceTextureData& textureData = m_inputDeviceTextureDictionary[ inputDevice ];
//************** AZERTY keyboard workaround ******************
#if defined RED_PLATFORM_WINPC
				if ( inputDevice == input::EInputDevice::KBD_MOUSE )
				{
					String keyString = GetKeyName( static_cast< Uint32 >( inputKeyId ) );
					keyString.RemoveWhiteSpaces();
					String keyName = s_keyPrefix + keyString;
					if ( GetInputKeyId( keyName ) == IK_None )
					{
						CName keyTexturePart = GetKeyTexturePart( keyString, isHold );
						if ( keyTexturePart != CName::NONE() )
						{
							outData.SetAtlas( textureData.m_atlas );
							outData.SetPart( keyTexturePart );
							return true;
						}
					}
				}
#endif
//************** AZERTY keyboard workaround ******************
				if( textureData.m_iconPartData.KeyExist( inputKeyId ) )
				{
					const InputIconPartData& iconData = textureData.m_iconPartData[ inputKeyId ];

					outData.SetAtlas( textureData.m_atlas );
					outData.SetPart( iconData.m_part );
					return true;
				}
			}
		}
		return false;
	}

	String InputKeyIconManager::GetInputKeyName( const InputKeyData& inputKey ) const
	{
		if( IsValidInputKey( inputKey ) )
		{
			String keyName;
			if( inputKey.IsHoldInput() )
			{
				keyName += s_holdInputNamePrefix;
			}
			keyName += m_keyIdToStringMap[ inputKey.GetRawInputKeyID() ];
			return keyName;
		}
		return s_invalidInputNameStr;
	}

	Int32 InputKeyIconManager::GetInputKeyId( const String& inputKeyName ) const
	{
		return m_keyStringToIdMap.KeyExist( inputKeyName ) ? m_keyStringToIdMap[inputKeyName] : IK_None;
	}

	Bool InputKeyIconManager::IsValidInputKey( const InputKeyData& inputKey ) const
	{
		return IsValidInputKey( inputKey.GetRawInputKeyID() );
	}

	Bool InputKeyIconManager::IsValidInputKey( Int32 inputKeyId ) const
	{
		return ( inputKeyId > IK_None && inputKeyId < ICK_Count );
	}

	Bool InputKeyIconManager::IsSupportedInputDevice( input::EInputDevice inputDevice ) const
	{
		return ( inputDevice != input::EInputDevice::INVALID && m_inputDeviceTextureDictionary.KeyExist( inputDevice ) );
	}

	input::EInputDevice InputKeyIconManager::GetInputDeviceByName( const CName& inputDeviceName ) const
	{
		// TODO: Use actual names from InputDevice
		if( inputDeviceName == RED_NAME_CONSTEXPR_NOREG( "keyboard" ) )
		{
			return input::EInputDevice::KBD_MOUSE;
		}
		else if( inputDeviceName == RED_NAME_CONSTEXPR_NOREG( "durango" ) )
		{
			return input::EInputDevice::DURANGO;
		}
		else if( inputDeviceName == RED_NAME_CONSTEXPR_NOREG( "xbox" ) )
		{
			return input::EInputDevice::XINPUT_PAD;
		}
		else if( inputDeviceName == RED_NAME_CONSTEXPR_NOREG( "orbis" ) )
		{
			return input::EInputDevice::ORBIS;
		}
		else if( inputDeviceName == RED_NAME_CONSTEXPR_NOREG( "steam" ) )
		{
			return input::EInputDevice::STEAM;
		}
		return input::EInputDevice::INVALID;
	}

	input::EInputDevice InputKeyIconManager::GetActiveInputDevice() const
	{
		return m_activeInputDevice;
	}

	void InputKeyIconManager::SetActiveInputDevice( input::EInputDevice inputDevice )
	{	
		if( m_activeInputDevice != inputDevice )
		{
			const input::EInputDevice lastInputDevice = m_activeInputDevice;

			m_activeInputDevice = inputDevice;
			OnInputDeviceChanged( lastInputDevice, m_activeInputDevice );
		}
	}

	/* static */ const String& InputKeyIconManager::GetInvalidInputNameString()
	{
		return s_invalidInputNameStr;
	}

	void InputKeyIconManager::LoadTextureDictionaryJSON()
	{
		if ( m_inputKeyIconDictionary.IsValid() == false )
			return;

		auto loadingToken = m_inputKeyIconDictionary.IssueLoadingRequest()->WaitUntilLoaded();
		THandle<JsonResource> jsonRes = Cast<JsonResource>( loadingToken );
		if ( jsonRes )
		{
			auto jsonProperties = Cast< InputKeyDictionaryJSON >( jsonRes->GetRoot() );
			for ( const InputDevicesMappingsJSON devicesMappings : jsonProperties->m_devicesMappings )
			{
				for ( const CName& deviceName : devicesMappings.m_devices )
				{
					const input::EInputDevice inputDevice = GetInputDeviceByName( deviceName );
					const Bool isSupportedDevice = IsSupportedInputDevice( inputDevice );
					RED_ERROR( isSupportedDevice, "[InputIconTextureDictionary] Device name is either not specified or not supported." );
					if ( isSupportedDevice )
					{
						for ( const InputIconMappingJSON& iconMapping : devicesMappings.m_mappings )
						{
							LoadInputDeviceIconMappings( iconMapping, m_inputDeviceTextureDictionary[inputDevice] );
						}
					}
				}
			}
		}
	}

//************** AZERTY keyboard workaround ******************
#if defined RED_PLATFORM_WINPC
	red::String InputKeyIconManager::GetKeyName( Uint32 virtualKey ) const
	{
		Uint32 scanCode = MapVirtualKey( virtualKey, MAPVK_VK_TO_VSC );

		// because MapVirtualKey strips the extended bit for some keys
		switch ( virtualKey )
		{
		case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: // arrow keys
		case VK_PRIOR: case VK_NEXT: // page up and page down
		case VK_END: case VK_HOME:
		case VK_INSERT: case VK_DELETE:
		case VK_DIVIDE: // numpad slash
		case VK_NUMLOCK:
		{
			scanCode |= 0x100; // set extended bit
			break;
		}
		}

		char keyName[ 50 ];
		if ( GetKeyNameTextA( scanCode << 16, keyName, sizeof( keyName ) ) != 0 )
		{
			return keyName;
		}
		else
		{
			return "[Error]";
		}
	}

	CName InputKeyIconManager::GetKeyTexturePart( const String& keyName, Bool isHold ) const
	{
		String texturePartName{};
		if ( keyName == "�" )
		{
			texturePartName ="kb_superscript2";
		}
		else if ( keyName == "&" )
		{
			texturePartName ="kb_ampersand";
		}
		else if ( keyName == "�" )
		{
			texturePartName ="kb_e_acute";
		}
		else if ( keyName == "\"" )
		{
			texturePartName ="kb_qutation_mark";
		}
		else if ( keyName == "(" )
		{
			texturePartName ="kb_left_parenthesis";
		}
		else if ( keyName == ")" )
		{
			texturePartName ="kbright_parenthesis";
		}
		else if ( keyName == "�" )
		{
			texturePartName ="kb_e_grave";
		}
		else if ( keyName == "_" )
		{
			texturePartName ="kb_low_line";
		}
		else if ( keyName == "�" )
		{
			texturePartName ="kb_c_cedilla";
		}
		else if ( keyName == "�" )
		{
			texturePartName ="kb_a_grave";
		}
		else if ( keyName == "^" )
		{
			texturePartName ="kb_circumflex_accent";
		}
		else if ( keyName == "$" )
		{
			texturePartName ="kb_dollar_sign";
		}
		else if ( keyName == "*" )
		{
			texturePartName ="kb_asterix";
		}
		else if ( keyName == "�" )
		{
			texturePartName ="kb_u_grave";
		}
		else if ( keyName == "!" )
		{
			texturePartName ="kb_exclamation_mark";
		}

		if ( texturePartName.Empty() == false )
		{
			if ( isHold )
			{
				texturePartName += "_hold";
			}
			return RED_NAME_NOREG( texturePartName.AsChar() );
		}

		return CName::NONE();
	}
#endif
//************** AZERTY keyboard workaround ******************

	InputKeyDictionaryJSON::InputKeyDictionaryJSON()
		: m_devicesMappings( PoolInk_System() )
	{
	}

	InputDevicesMappingsJSON::InputDevicesMappingsJSON()
		: m_devices( PoolInk_System() )
		, m_mappings( PoolInk_System() )
	{
	}

}	// namespace ink
