/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "icuConversions.h"
#include <cstdio>
#include <cstring>

#ifndef INK_DISABLE_ICU

namespace text
{
	red::Utf16String ConvertToRedString( const icu::UnicodeString& icuString )
	{
		red::Utf16String destinationRedString;
		ConvertToRedString( icuString, destinationRedString );
		return destinationRedString;
	}

	void ConvertToRedString( const icu::UnicodeString& icuString, red::Utf16String& redString )
	{
		ICUStringConverter converter;
		converter.ConvertToRedString( icuString, 0, icuString.length(), redString );
	}

	icu::UnicodeString ConvertToICUString( const red::Utf16String& redString )
	{
		icu::UnicodeString destinationIcuString;
		ConvertToICUString( redString, destinationIcuString );
		return destinationIcuString;
	}

	icu::UnicodeString ConvertToICUString( const String& string )
	{
		red::Utf16String redString = ConvertToRedString( string );
		icu::UnicodeString destinationIcuString;
		ConvertToICUString( redString, destinationIcuString );
		return destinationIcuString;
	}

	void ConvertToICUString( const red::Utf16String& redString, icu::UnicodeString& icuString )
	{
		ICUStringConverter converter;
		converter.ConvertToICUString( redString, icuString, true );
	}

	red::Utf16String ConvertToRedString( const icu::UnicodeString& icuString, Int32 startIndex, Int32 length )
	{
		red::Utf16String redString;

		ICUStringConverter converter;
		converter.ConvertToRedString( icuString, startIndex, length, redString );

		return redString;
	}

	red::Utf16String ConvertToRedString( const String & string )
	{
		ICUStringConverter converter( "UTF-8" );
		icu::UnicodeString unicodeString;
		converter.ConvertToICUString( string.AsChar(),0,string.Length(), unicodeString );
		return ConvertToRedString( unicodeString );
	}

	icu::UnicodeString ConvertToICUString( const UniChar* redString, Int32 startIndex, Int32 length )
	{
		icu::UnicodeString icuString;

		ICUStringConverter converter;
		converter.ConvertToICUString( redString, startIndex, length, icuString );

		return icuString;
	}

	Int32 GetICUStringIndex( const red::Utf16String& redString, Int32 startIndex, Int32 length )
	{
		if ( startIndex < 0 )
		{
			return -1;
		}

		if ( length < 0 )
		{
			return -1;
		}

		if ( length == 0 )
		{
			return 0;
		}

		const icu::UnicodeString icuString = ConvertToICUString( redString.AsChar(), startIndex, length );
		return icuString.length();
	}

	Int32 GetRedStringIndex( const icu::UnicodeString& icuString, Int32 startIndex, Int32 length )
	{
		if ( startIndex < 0 )
		{
			return -1;
		}

		if ( length < 0 )
		{
			return -1;
		}

		if ( length == 0 )
		{
			return 0;
		}

		const red::Utf16String redString = ConvertToRedString( icuString, startIndex, length );
		return redString.Length();
	}

	ICUStringConverter::ICUStringConverter( const char * encoding )
		: m_icuConverter( nullptr )
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;
		m_icuConverter = ucnv_open( encoding, &icuErrorCode );
		
		RED_ICU_CHECK( icuErrorCode );
#if defined( RED_PLATFORM_WEB )
		// [npcblack] probe: ucnv_open needs the ICU alias table from the data package. A failed open would leave
		// every red->ICU conversion empty (rich-text markup regex then finds no <Input/> tags). Count-limited.
		static int s_reports = 0;
		if ( ( m_icuConverter == nullptr || U_FAILURE( icuErrorCode ) ) && s_reports < 8 )
		{
			++s_reports;
			std::fprintf( stderr, "CP2077 npcblack ICU: ucnv_open(%s) failed status=%s converter=%p\n", encoding ? encoding : "(null)", u_errorName( icuErrorCode ), (void*)m_icuConverter );
		}
#endif
	}

	ICUStringConverter::~ICUStringConverter()
	{
		ucnv_close( m_icuConverter );
	}

	void ICUStringConverter::ConvertToICUString( const red::Utf16String& redString, icu::UnicodeString& icuString, Bool shouldEndWithNull )
	{
		ConvertToICUString( redString.AsChar(), 0, redString.Length(), icuString, shouldEndWithNull );
	}

	void ICUStringConverter::ConvertToICUString( const UniChar* redString, Int32 startIndex, Int32 length, icu::UnicodeString& icuString, Bool shouldEndWithNull )
	{
		if ( length <= 0 )
		{
			icuString.remove();
			return;
		}

#if defined( RED_PLATFORM_WEB )
		// [npcblack] UTF-16LE -> UChar is an identity copy on little-endian wasm; only used when ucnv_open failed.
		if ( m_icuConverter == nullptr )
		{
			UChar* direct = icuString.getBuffer( length + 1 );
			std::memcpy( direct, redString + startIndex, length * sizeof( UChar ) );
			if ( shouldEndWithNull ) direct[ length ] = 0;
			icuString.releaseBuffer( length );
			return;
		}
#endif
		ucnv_reset( m_icuConverter );

		const Int32 sourceSizeInBytes = length * sizeof( UChar );

		const Int32 destinationCapacity = length * 2;
		UChar* internalDestinationBuffer = icuString.getBuffer( destinationCapacity );

		UErrorCode icuErrorCode = U_ZERO_ERROR;
		const Int32 realDestinationLength = ucnv_toUChars( m_icuConverter, internalDestinationBuffer, destinationCapacity, reinterpret_cast< const char* >( redString + startIndex ), sourceSizeInBytes, &icuErrorCode );
		RED_ICU_CHECK( icuErrorCode );

		if ( shouldEndWithNull )
		{
			internalDestinationBuffer[ realDestinationLength ] = 0;
		}

		icuString.releaseBuffer( realDestinationLength );
	}

	void ICUStringConverter::ConvertToICUString( const char* redString, Int32 startIndex, Int32 length, icu::UnicodeString& icuString, Bool shouldEndWithNull )
	{
		if( length <= 0 )
		{
			icuString.remove();
			return;
		}

#if defined( RED_PLATFORM_WEB )
		if ( m_icuConverter == nullptr ) // [npcblack] UTF-8 fallback when ucnv_open failed
		{
			icuString = icu::UnicodeString::fromUTF8( icu::StringPiece( redString + startIndex, length ) );
			return;
		}
#endif
		ucnv_reset( m_icuConverter );

		const Int32 sourceSizeInBytes = length * sizeof( char );

		const Int32 destinationCapacity = length * 2;
		UChar* internalDestinationBuffer = icuString.getBuffer( destinationCapacity );

		UErrorCode icuErrorCode = U_ZERO_ERROR;
		const Int32 realDestinationLength = ucnv_toUChars( m_icuConverter, internalDestinationBuffer, destinationCapacity,( redString + startIndex ), sourceSizeInBytes, &icuErrorCode );
		RED_ICU_CHECK( icuErrorCode );

		if( shouldEndWithNull )
		{
			internalDestinationBuffer[ realDestinationLength ] = 0;
		}

		icuString.releaseBuffer( realDestinationLength );
	}

	void ICUStringConverter::ConvertToRedString( const icu::UnicodeString& icuString, Int32 sourceStartIndex, Int32 sourceLength, red::Utf16String& redString )
	{
		if ( sourceLength <= 0 )
		{
			redString.Clear();
			return;
		}

		redString = red::Utf16String{ reinterpret_cast< const red::CodeUnit16* >( icuString.getBuffer() ), static_cast< Uint32 >( sourceLength ) };
	}

}

#endif	// INK_DISABLE_ICU
