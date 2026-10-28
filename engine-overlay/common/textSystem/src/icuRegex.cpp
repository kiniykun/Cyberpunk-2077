/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "icuRegex.h"
#include "icuConversions.h"
#include <cstdio>
#include "textLayoutSettings.h"

RED_NO_EMPTY_FILE();

#ifndef INK_DISABLE_ICU
#include "unicode/urename.h"

namespace text
{
	//************************************ Regex Pattern ************************************

	RegexPattern::RegexPattern()
		: m_icuRegexPattern( nullptr )
	{
	}

	RegexPattern::RegexPattern( const red::Utf16String& inputString )
	{
		CreateICURegexPattern( inputString );
	}

	void RegexPattern::CreateICURegexPattern( const red::Utf16String& inputString )
	{
		icu::UnicodeString icuInputString = ConvertToICUString( inputString );

		UErrorCode icuStatus = U_ZERO_ERROR;
		m_icuRegexPattern = red::SharedPtr< icu::RegexPattern, text::PoolText_ICU_Internal >( icu::RegexPattern::compile( icuInputString, 0, icuStatus ) );
		RED_ICU_CHECK( icuStatus );
#if defined( RED_PLATFORM_WEB )
		// [npcblack] probe: RED_ICU_CHECK is a no-op in FINAL; report failed/empty markup patterns (count-limited).
		static int s_reports = 0;
		if ( s_reports < 12 && ( U_FAILURE( icuStatus ) || !m_icuRegexPattern || icuInputString.length() == 0 ) )
		{
			++s_reports;
			char text[ 96 ]; Uint32 n = 0;
			for ( ; n < inputString.Length() && n < 95; ++n ) { const Uint32 c = inputString[ n ]; text[ n ] = ( c >= 32 && c < 127 ) ? char( c ) : '?'; }
			text[ n ] = 0;
			std::fprintf( stderr, "CP2077 npcblack ICU: regex compile status=%s pattern=%p redLen=%u icuLen=%d text=[%s]\n", u_errorName( icuStatus ), (void*)m_icuRegexPattern.Get(), (unsigned)inputString.Length(), (int)icuInputString.length(), text );
		}
#endif
	}

	RegexPattern::~RegexPattern()
	{
		m_icuRegexPattern.Reset();
	}

	red::SharedPtr< icu::RegexPattern, text::PoolText_ICU_Internal > RegexPattern::GetICURegexPattern() const
	{
		return m_icuRegexPattern;
	}

	//************************************ Regex Matcher ************************************

	RegexMatcher::RegexMatcher( const RegexPattern& regexPattern, const red::Utf16String& inputString )
		: m_icuString( ConvertToICUString( inputString ) )
		, m_sourceString( inputString )
	{
		CreateICURegexMatcher( regexPattern.GetICURegexPattern(), &m_icuString );
	}

	void RegexMatcher::CreateICURegexMatcher(red::SharedPtr< icu::RegexPattern, text::PoolText_ICU_Internal > icuRegexPattern, const icu::UnicodeString* inputString )
	{
		m_icuRegexMatcher = nullptr;
		if ( icuRegexPattern && inputString )
		{
			UErrorCode icuStatus = U_ZERO_ERROR;
			m_icuRegexMatcher = red::SharedPtr< icu::RegexMatcher, text::PoolText_ICU_Internal >( icuRegexPattern->matcher( *inputString, icuStatus ) );
			RED_ICU_CHECK( icuStatus );
		}
	}

	RegexMatcher::~RegexMatcher()
	{
		m_icuRegexMatcher.Reset();
	}

	const red::Utf16String& RegexMatcher::GetSourceString() const
	{
		return m_sourceString;
	}


	Bool RegexMatcher::Find(const Int32 index)
	{
		UErrorCode icuStatus = U_ZERO_ERROR;
		Bool result = m_icuRegexMatcher != nullptr && m_icuRegexMatcher->find(index, icuStatus) != 0;
		RED_ICU_CHECK( icuStatus );
		return result;
	}

	Bool RegexMatcher::FindNext()
	{
		return m_icuRegexMatcher != nullptr && m_icuRegexMatcher->find() != 0;
	}

	Int32 RegexMatcher::GetMatchBegin()
	{
		UErrorCode icuStatus = U_ZERO_ERROR;
		Int32 result = m_icuRegexMatcher != nullptr ? m_icuRegexMatcher->start( icuStatus ) : INDEX_NONE;
		RED_ICU_CHECK( icuStatus );
		return result;
	}

	Int32 RegexMatcher::GetMatchEnd()
	{
		UErrorCode icuStatus = U_ZERO_ERROR;
		Int32 result = m_icuRegexMatcher != nullptr ? m_icuRegexMatcher->end( icuStatus ) : INDEX_NONE;
		RED_ICU_CHECK( icuStatus );
		return result;
	}

	Int32 RegexMatcher::GetGroupBegin( const Int32 index )
	{
		UErrorCode icuStatus = U_ZERO_ERROR;
		Int32 result = m_icuRegexMatcher != nullptr ? m_icuRegexMatcher->start( index, icuStatus ) : INDEX_NONE;
		RED_ICU_CHECK( icuStatus );
		return result;
	}

	Int32 RegexMatcher::GetGroupEnd( const Int32 index )
	{
		UErrorCode icuStatus = U_ZERO_ERROR;
		Int32 result = m_icuRegexMatcher != nullptr ? m_icuRegexMatcher->end( index, icuStatus ) : INDEX_NONE;
		RED_ICU_CHECK( icuStatus );
		return result;
	}

	red::Utf16String RegexMatcher::GetGroup( const Int32 index )
	{
		Int32 groupBegin = 0;
		Int32 groupEnd = 0;

		if ( m_icuRegexMatcher != nullptr )
		{
			UErrorCode icuStatus = U_ZERO_ERROR;
			groupBegin = m_icuRegexMatcher->start( index, icuStatus );
			RED_ICU_CHECK( icuStatus );
			groupEnd = m_icuRegexMatcher->end(index, icuStatus);
			RED_ICU_CHECK( icuStatus );
		}

		groupBegin = math::Max( 0, groupBegin );
		groupEnd = math::Max( groupBegin, groupEnd );

		return red::utf16::SubStr( m_sourceString, groupBegin, groupEnd - groupBegin );
	}

	Int32 RegexMatcher::GetBeginLimit()
	{
		return m_icuRegexMatcher != nullptr ? m_icuRegexMatcher->regionStart() : INDEX_NONE;
	}

	Int32 RegexMatcher::GetEndLimit()
	{
		return m_icuRegexMatcher != nullptr ? m_icuRegexMatcher->regionEnd() : INDEX_NONE;
	}

	void RegexMatcher::SetLimits( const Int32 beginIndex, const Int32 endIndex )
	{
		if ( m_icuRegexMatcher == nullptr )
			return;

		UErrorCode icuStatus = U_ZERO_ERROR;
		m_icuRegexMatcher->region( beginIndex, endIndex, icuStatus );
		RED_ICU_CHECK( icuStatus );
	}

	void RegexMatcher::Reset(const Int32 index)
	{
		if (m_icuRegexMatcher == nullptr)
			return;

		UErrorCode icuStatus = U_ZERO_ERROR;
		m_icuRegexMatcher->reset( index, icuStatus );
		RED_ICU_CHECK( icuStatus );
	}

}	// namespace text

#endif	// INK_DISABLE_ICU