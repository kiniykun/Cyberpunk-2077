/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "tweakDBLexer.h"
#include "tweakDBErrorLog.h"

#if defined( RED_PLATFORM_WIN32 ) || defined( RED_PLATFORM_WIN64 ) || defined( RED_PLATFORM_DURANGO )
#	pragma warning( push )
#	pragma warning( disable : 4005 )
#		include "../../../../temp/gen/gameTweakDB/tdb_flex.cxx"
#	pragma warning( pop )
#elif defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#pragma clang diagnostic ignored "-Wunneeded-internal-declaration"
#pragma clang diagnostic ignored "-Wunused-function"
#		include "../../../../temp/gen/gameTweakDB/tdb_flex.cxx"
#pragma clang diagnostic pop
#endif


#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif

game::data::LexerListener::LexerListener( const String& filename, red::DynArray< Token >& tokens, TweakDBErrorLog& errorLog )
	: m_filename( filename )
	, m_tokens( tokens )
	, m_errorLog( errorLog )
{
}

void game::data::LexerListener::OnComment( const LexerState& lexerState )
{
	const AnsiChar* text = lexerState.m_source + lexerState.m_sequenceStart.m_byte;
	const Uint32 length = lexerState.m_currentContext.m_byte - lexerState.m_sequenceStart.m_byte;

	PushIgnoredToken( red::String( text, length ) );
}

void game::data::LexerListener::OnToken( const LexerState& lexerState, const TokenType token )
{
	const AnsiChar* text = lexerState.m_source + lexerState.m_tokenStart.m_byte;
	const Uint32 length = lexerState.m_currentContext.m_byte - lexerState.m_tokenStart.m_byte;

	m_tokens.PushBack( Token( token, red::String( text, length ), lexerState.m_tokenStart ) );
}

void game::data::LexerListener::OnSequence( const LexerState& lexerState, const TokenType token )
{
	const AnsiChar* text = lexerState.m_source + lexerState.m_sequenceStart.m_byte + 1;
	const Uint32 length = lexerState.m_currentContext.m_byte - lexerState.m_sequenceStart.m_byte - 2;

	m_tokens.PushBack( Token( token, red::String( text, length ), lexerState.m_sequenceStart ) );
}

void game::data::LexerListener::OnError( const LexerState& lexerState )
{
	m_errorLog.LogParsingError( m_filename, lexerState.m_lineStart.m_line, lexerState.m_lineStart.m_character, ErrorMessages::UNKNOWN_TOKEN );
}

void game::data::LexerListener::OnWhitespace( const LexerState& lexerState )
{
	const AnsiChar* text = lexerState.m_source + lexerState.m_tokenStart.m_byte;
	const Uint32 length = lexerState.m_currentContext.m_byte - lexerState.m_tokenStart.m_byte;

	PushIgnoredToken( red::String( text, length ) );
}

void game::data::LexerListener::OnNewLine( const LexerState& lexerState )
{
	PushIgnoredToken( "\n" );
}

void game::data::LexerListener::PushIgnoredToken( const String& string )
{
	if ( !m_tokens.Empty() )
	{
		m_tokens.Back().m_ignoredTokens.PushBack( std::move( string ) );
	}
}

game::data::Lexer::Lexer( const String& content, const String& fileName )
	: m_fileName( fileName )
	, m_content( content.AsChar() )
{
}

Bool game::data::Lexer::Tokenize( red::DynArray< Token >& outTokens, TweakDBErrorLog& errorLog ) const
{
	LexerListener listener( m_fileName, outTokens, errorLog );

	InternalState lexData( m_content, listener );

	yyscan_t scanner;
	yylex_init_extra( &lexData, &scanner );

	YY_BUFFER_STATE buf = tdblex_scan_string( m_content, scanner );
	yylex( scanner );

	yy_delete_buffer( buf, scanner );
	yylex_destroy( scanner );

	return true;
}

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif
