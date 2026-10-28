/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "icuRegexCache.h"
#include "textUtils.h"
#include <cstdio>

#ifndef INK_DISABLE_ICU

namespace
{
	red::StaticArray< text::RegexPattern, text::RegexPatternCache::Type::Count > s_regexPatterns;

	red::Utf16String GetEscapeSequenceString(const red::ArraySpan< const red::Utf16String >& escapeSeq)
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		red::Utf16String escapeSequenceString;

		for (const auto& escSeq : escapeSeq)
		{
			if (!(escapeSequenceString.Empty()))
			{
				escapeSequenceString.Append(red::Utf16String{ "|" });
			}

			escapeSequenceString.Append(red::Utf16String{ "(" });
			escapeSequenceString.Append(red::Utf16String{ "&" });
			escapeSequenceString.Append(escSeq);
			escapeSequenceString.Append(red::Utf16String{ ";" });
			escapeSequenceString.Append(red::Utf16String{ ")" });
		}

		return escapeSequenceString;
	}
}

namespace text
{
	void RegexPatternCache::Initialize()
	{
		const EscapeHelper escapeHelper;

		s_regexPatterns.Resize(text::RegexPatternCache::Type::Count);

#if defined( RED_PLATFORM_WEB )
		// [npcblack] In the web build the wide literal patterns arrived truncated (ICU: U_REGEX_MISMATCHED_PAREN for
		// lengths 66/52 and U_REGEX_MISSING_CLOSE_BRACKET for 16 instead of 75/70/29), so no rich-text tag was ever
		// parsed and tutorials showed raw <Input .../> markup. Same pattern text, built from explicit-length ASCII
		// literals (code units widened one by one) so the length never depends on a terminator scan of the wide data.
		// The diagnostic below reports the wide literal's current length and the first differing code unit.
		auto makePattern = []( const UniChar* wide, const char* ascii, Uint32 length ) -> red::Utf16String
		{
			red::CodeUnit16 units[ 128 ] = {};
			RED_ASSERT( length < 128 );
			Uint32 firstDiff = length;
			for ( Uint32 i = 0; i < length; ++i )
			{
				units[ i ] = static_cast< red::CodeUnit16 >( static_cast< unsigned char >( ascii[ i ] ) );
				if ( firstDiff == length && static_cast< red::CodeUnit16 >( wide[ i ] ) != units[ i ] ) firstDiff = i;
			}
			if ( firstDiff != length )
				std::fprintf( stderr, "CP2077 npcblack regex literal: wide literal differs at %u/%u (unit=0x%04x expected '%c')\n",
					unsigned( firstDiff ), unsigned( length ), unsigned( static_cast< red::CodeUnit16 >( wide[ firstDiff ] ) ), ascii[ firstDiff ] );
			return red::Utf16String( static_cast< const red::CodeUnit16* >( units ), length );
		};
#define NPCBLACK_PATTERN( lit ) makePattern( TXT( lit ), lit, Uint32( sizeof( lit ) - 1 ) )
		s_regexPatterns[Type::MarkupTag] = RegexPattern( NPCBLACK_PATTERN( "<([\\w\\d\\.-]+)((?: (?:[\\w\\d\\.-]+(?:=(?>\".*?\"))?))+)?(?:(?:/>)|(?:>(.*?)</>))" ) );
		s_regexPatterns[Type::MarkupAttribute] = RegexPattern( NPCBLACK_PATTERN( "([\\w\\d\\.]+)(?:=(?>\"(.*?)\"))?" ) );
		s_regexPatterns[Type::MarkupInput] = RegexPattern( NPCBLACK_PATTERN( "<(Input)((?: (?:[\\w\\d\\.-]+(?:=(?>\".*?\"))?))+)?(?:(?:/>)|(?:>(.*?)</>))" ) );
		s_regexPatterns[Type::EscapedCharacters] = RegexPattern( GetEscapeSequenceString(escapeHelper.m_escapeSequence) );
		s_regexPatterns[Type::RRGGBBAA] = RegexPattern( NPCBLACK_PATTERN( "(?:#)?(?:[0-9a-fA-F]{2}){3,4}" ) );
		s_regexPatterns[Type::All] = RegexPattern( NPCBLACK_PATTERN( ".*" ) );
#undef NPCBLACK_PATTERN
		{
			Uint32 ok = 0;
			for ( Uint32 i = 0; i < s_regexPatterns.Size(); ++i ) ok += s_regexPatterns[ i ].GetICURegexPattern() ? 1 : 0;
			std::fprintf( stderr, "CP2077 npcblack regex cache: %u/%u markup patterns compiled\n", unsigned( ok ), unsigned( s_regexPatterns.Size() ) );
		}
#else
		s_regexPatterns[Type::MarkupTag] = RegexPattern(red::Utf16String{ TXT("<([\\w\\d\\.-]+)((?: (?:[\\w\\d\\.-]+(?:=(?>\".*?\"))?))+)?(?:(?:/>)|(?:>(.*?)</>))") });
		s_regexPatterns[Type::MarkupAttribute] = RegexPattern(red::Utf16String{ TXT("([\\w\\d\\.]+)(?:=(?>\"(.*?)\"))?") });
		s_regexPatterns[Type::MarkupInput] = RegexPattern(red::Utf16String{ TXT("<(Input)((?: (?:[\\w\\d\\.-]+(?:=(?>\".*?\"))?))+)?(?:(?:/>)|(?:>(.*?)</>))") });
		s_regexPatterns[Type::EscapedCharacters] = RegexPattern( GetEscapeSequenceString(escapeHelper.m_escapeSequence) );
		s_regexPatterns[Type::RRGGBBAA] = RegexPattern(red::Utf16String{ TXT("(?:#)?(?:[0-9a-fA-F]{2}){3,4}") });
		s_regexPatterns[Type::All] = RegexPattern(red::Utf16String{ TXT(".*") });
#endif
	}

	void RegexPatternCache::Uninitialize()
	{
		s_regexPatterns.Clear();
	}

	const text::RegexPattern& RegexPatternCache::Get(Type type)
	{
		const Uint32 typeIndex = static_cast<Uint32>(type);
		if (typeIndex < s_regexPatterns.Size())
		{
			return s_regexPatterns[typeIndex];
		}

		const Uint32 allIndex = static_cast<Uint32>(Type::All);
		return s_regexPatterns[Type::All];
	}

}	// namespace text

#else

RED_NO_EMPTY_FILE();

#endif	// INK_DISABLE_ICU