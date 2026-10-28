/**
* Copyright (C) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "richTextMarkupParser.h"
#include "textDecoratorInterface.h"
#include "richTextLayoutFormatter.h"
#include "textUtils.h"
#include "textLayoutSettings.h"
#include "icuTextUtils.h"
#include "icuRegexCache.h"
#include "icuConversions.h"
#include <cstdio>

namespace text
{


	RichTextMarkupParser::RichTextMarkupParser()
	{
	}

	void RichTextMarkupParser::Process( red::DynArray< ParsedTextLineResults >& results, const red::Utf16String& input, red::Utf16String& output )
	{
#ifndef INK_DISABLE_ICU
		red::DynArray< TextRange > linesRanges{ text::PoolText_Markup() };

		red::Utf16String& modifiableInput = const_cast< red::Utf16String& >( input );
		RemoveSpacesSurroundingInputRuns( modifiableInput);
		CalculateLineRangesFromString( modifiableInput, linesRanges );
		Parse( modifiableInput, linesRanges, results );
		HandleEscapeSequences( modifiableInput, results, output );
#if defined( RED_PLATFORM_WEB )
		// [npcblack] probe: once per call while < 10 reports, for inputs that contain an <Input markup tag.
		{
			static int s_reports = 0;
			const red::Utf16String needle{ "<Input" };
			if ( s_reports < 10 && red::utf16::Find( modifiableInput, needle ) < modifiableInput.Length() )
			{
				++s_reports;
				Uint32 tags = 0; Uint32 runs = 0;
				for ( const ParsedTextLineResults& line : results ) for ( const ParsedTextRunResults& run : line.m_tags ) { ++runs; if ( !run.m_tag.Empty() ) ++tags; }
				const icu::UnicodeString probe = ConvertToICUString( modifiableInput );
				std::fprintf( stderr, "CP2077 npcblack markup: inputLen=%u icuLen=%d lines=%u runs=%u tags=%u outLen=%u\n", (unsigned)modifiableInput.Length(), (int)probe.length(), (unsigned)results.Size(), runs, tags, (unsigned)output.Length() );
			}
		}
#endif
#endif
	}

	void RichTextMarkupParser::RemoveSpacesSurroundingInputRuns( red::Utf16String& input )
	{
#ifndef INK_DISABLE_ICU
		Int32 lastTagEnd = 0;
		red::DynArray< TextRange > inputTagResults{ text::PoolText_Markup() };
		RegexMatcher tagRegexMatcher(text::RegexPatternCache::Get(text::RegexPatternCache::Type::MarkupInput), input );
		while ( tagRegexMatcher.Find( lastTagEnd ) )
		{
			TextRange inputTagRange( tagRegexMatcher.GetMatchBegin(), tagRegexMatcher.GetMatchEnd() );
			inputTagResults.PushBack( inputTagRange );
			lastTagEnd = inputTagRange.End();
		}

		Uint32 indexOffset = 0;
		red::CodeUnit16 spaceChar = red::Utf16String{ L"\u0020" }[ 0 ];
		for ( TextRange& tagRange : inputTagResults )
		{
			Int32 begin = tagRange.Begin() - indexOffset;
			if ( begin > 0 && input[ begin - 1 ] == spaceChar )
			{
				input.Remove( begin - 1, begin );
				indexOffset++;
			}
			Int32 end = tagRange.End() - indexOffset;
			if ( end > 0 && end < static_cast< Int32 >( input.Length() ) && input[ end ] == spaceChar )
			{
				input.Remove( end, end + 1 );
				indexOffset++;
			}
		}
#endif
	}

	void RichTextMarkupParser::Parse( const red::Utf16String& input, red::DynArray< TextRange >& lineRanges, red::DynArray< ParsedTextLineResults >& lineParseResultArray ) const
	{
#ifndef INK_DISABLE_ICU
		const red::Utf16String emptyString("");

		RegexMatcher tagRegexMatcher( text::RegexPatternCache::Get( text::RegexPatternCache::Type::MarkupTag ), input);

		for ( Uint32 i = 0; i < lineRanges.Size(); ++i )
		{
			ParsedTextLineResults parsedLinesResults;
			parsedLinesResults.m_lineRange = lineRanges[ i ];

			tagRegexMatcher.SetLimits( parsedLinesResults.m_lineRange.Begin(), parsedLinesResults.m_lineRange.End() );

			Int32 lastRunEnd = parsedLinesResults.m_lineRange.Begin();
			while ( tagRegexMatcher.Find( lastRunEnd ) )
			{
				Int32 tagBegin = tagRegexMatcher.GetMatchBegin();
				Int32 tagEnd = tagRegexMatcher.GetMatchEnd();

				// Match is outside this line
				// SetLimits seems to not working properly
				if ( tagBegin >= parsedLinesResults.m_lineRange.End() )
				{
					if ( lastRunEnd != parsedLinesResults.m_lineRange.End() )
					{
						ParsedTextRunResults interveningParsedResults( emptyString, TextRange( lastRunEnd, parsedLinesResults.m_lineRange.End() ) );
						parsedLinesResults.m_tags.EmplaceBack(interveningParsedResults);
					}

					lastRunEnd = parsedLinesResults.m_lineRange.End();
					break;
				}

				TextRange singleTagRange(tagBegin, tagEnd);

				Int32 tagNameBegin = tagRegexMatcher.GetGroupBegin( 1 );
				Int32 tagNameEnd = tagRegexMatcher.GetGroupEnd( 1 );

				red::Utf16String tagName = red::utf16::SubStr( input, tagNameBegin, tagNameEnd - tagNameBegin );

				Int32 attributesBegin = tagRegexMatcher.GetGroupBegin( 2 );
				Int32 attributesEnd = tagRegexMatcher.GetGroupEnd( 2 );

				Int32 contentBegin = tagRegexMatcher.GetGroupBegin( 3 );
				Int32 contentEnd = tagRegexMatcher.GetGroupEnd( 3 );

				TextRange content( contentBegin, contentEnd );

				red::Map< red::Utf16String, TextRange > attribs{ text::PoolText_Markup() };

				if ( attributesBegin != INDEX_NONE && attributesEnd != INDEX_NONE )
				{
					RegexMatcher attribRegexMatcher( text::RegexPatternCache::Get( text::RegexPatternCache::Type::MarkupAttribute ), input );
					attribRegexMatcher.SetLimits( attributesBegin, attributesEnd );

					while ( attribRegexMatcher.FindNext() )
					{
						Int32 attribKeyBegin = attribRegexMatcher.GetGroupBegin( 1 );
						Int32 attribKeyEnd = attribRegexMatcher.GetGroupEnd( 1 );

						Int32 attribValueBegin = attribRegexMatcher.GetGroupBegin( 2 );
						Int32 attribValueEnd = attribRegexMatcher.GetGroupEnd( 2 );

						attribs.Emplace( red::utf16::SubStr( input, attribKeyBegin, attribKeyEnd - attribKeyBegin ), TextRange( attribValueBegin, attribValueEnd ) );
					}
				}

				TextRange interveningRange(lastRunEnd, tagBegin);
				if ( interveningRange.IsEmpty() == false )
				{
					ParsedTextRunResults interveningParsedResults(emptyString, interveningRange );
					lastRunEnd = tagBegin;
					parsedLinesResults.m_tags.EmplaceBack( interveningParsedResults );
				}

				ParsedTextRunResults singleParsedTagResult( tagName, singleTagRange, content );
				singleParsedTagResult.m_attributes = attribs;
				parsedLinesResults.m_tags.EmplaceBack( singleParsedTagResult );
				lastRunEnd = tagEnd;
			}

			TextRange interveningRange(lastRunEnd, parsedLinesResults.m_lineRange.End() );
			if ( !interveningRange.IsEmpty() )
			{
				ParsedTextRunResults interveningParsedResults( emptyString, interveningRange );
				lastRunEnd = parsedLinesResults.m_lineRange.End();
				parsedLinesResults.m_tags.EmplaceBack( interveningParsedResults );
			}

			if ( parsedLinesResults.m_tags.Size() == 0 )
			{
				ParsedTextRunResults emptyRunResults(emptyString, parsedLinesResults.m_lineRange );
				lastRunEnd = emptyRunResults.m_tagRange.End();
				parsedLinesResults.m_tags.EmplaceBack( emptyRunResults );
			}

			lineParseResultArray.EmplaceBack( parsedLinesResults );
		}
#endif
	}

	void RichTextMarkupParser::HandleEscapeSequences( const red::Utf16String& input, red::DynArray<ParsedTextLineResults>& lineParseResultArray, red::Utf16String& output ) const
	{
#ifndef INK_DISABLE_ICU
		for (Uint32 i = 0; i < lineParseResultArray.Size(); ++i)
		{
			ParsedTextLineResults& parsedLineResults = lineParseResultArray[ i ];

			parsedLineResults.m_lineRange.Begin( output.Length() );

			for ( Uint32 j = 0; j < parsedLineResults.m_tags.Size(); ++j )
			{
				if ( input == output )
				{
					break;
				}

				ParsedTextRunResults& parsedTagResult = parsedLineResults.m_tags[ j ];
				RegexMatcher escapeSequenceRegexMatcher(text::RegexPatternCache::Get(text::RegexPatternCache::Type::EscapedCharacters), input );

				red::DynArray<Int32*> indexesToUpdate{ PoolText_Markup() };
				indexesToUpdate.EmplaceBack( parsedTagResult.m_tagRange.BeginPtr() );

				const red::Map< red::Utf16String, TextRange >& attributes = parsedTagResult.m_attributes;
				for ( red::Map< red::Utf16String, TextRange >::ElementType elm : attributes )
				{
					TextRange& range = elm.Value();
					if (!range.IsEmpty())
					{
						indexesToUpdate.EmplaceBack(range.BeginPtr());
						indexesToUpdate.EmplaceBack(range.EndPtr());
					}
				}
					
				if ( parsedTagResult.m_content.Begin() != INDEX_NONE && parsedTagResult.m_content.End() != INDEX_NONE )
				{
					indexesToUpdate.EmplaceBack( parsedTagResult.m_content.BeginPtr() );
					indexesToUpdate.EmplaceBack( parsedTagResult.m_content.EndPtr() );
				}
				indexesToUpdate.EmplaceBack( parsedTagResult.m_tagRange.EndPtr() );

				std::sort(indexesToUpdate.Begin(), indexesToUpdate.End(), [this](const Int32* a, const Int32* b)
				{
					return ( *a ) < ( *b );
				});

				{
					const auto GetStringWithoutEscapeSeq = [&]()
					{
						Int32 lastIndex = escapeSequenceRegexMatcher.GetBeginLimit();
						while ( escapeSequenceRegexMatcher.FindNext() )
						{
							output.Append( red::utf16::SubStr( input, lastIndex, escapeSequenceRegexMatcher.GetMatchBegin() - lastIndex ) );
							lastIndex = escapeSequenceRegexMatcher.GetMatchBegin();

							const EscapeHelper escapeHelper;

							for (Uint32 k = 0; k < escapeHelper.m_escapeSequence.Size(); ++k)
							{
								const Int32 groupIndex = 1 + k;
								const Int32 escSeqBegin = escapeSequenceRegexMatcher.GetGroupBegin( groupIndex );
								const Int32 escSeqEnd = escapeSequenceRegexMatcher.GetGroupEnd( groupIndex );

								if (escSeqBegin != INDEX_NONE && escSeqEnd != INDEX_NONE)
								{
									output.Append(escapeHelper.m_unescapeChars[ k ] );
									break;
								}
							}

							lastIndex = escapeSequenceRegexMatcher.GetMatchEnd();
						}

						output.Append( red::utf16::SubStr( input, lastIndex, escapeSequenceRegexMatcher.GetEndLimit() - lastIndex ) );
					};

					Uint32 k;
					for ( k = 0; k + 1 < indexesToUpdate.Size(); ++k )
					{
						escapeSequenceRegexMatcher.SetLimits( *( indexesToUpdate[k] ), *( indexesToUpdate[ k + 1 ] ) );
						*( indexesToUpdate[ k ] ) = output.Length();
						GetStringWithoutEscapeSeq();
					}
					*( indexesToUpdate[ k ] ) = output.Length();
				}
			}

			// Restored new line
			if( i < lineParseResultArray.Size() - 1)
				output.Append('\n');

			parsedLineResults.m_lineRange.End( output.Length() );
		}
#endif
	}

	RichTextMarkupWriter::RichTextMarkupWriter() = default;

	void RichTextMarkupWriter::Write( const red::DynArray< RichTextLine >& lines, red::Utf16String& outputString )
	{
		for ( Uint32 lineIdx = 0; lineIdx < lines.Size(); ++lineIdx )
		{
			const RichTextLine& line = lines[ lineIdx ];

			if ( lineIdx > 0 )
			{
				outputString.Append( red::Utf16String{ "\r\n" } );
			}

			for (const SingleRichTextRun& run : line.m_runs)
			{
				const Bool hasTag = run.m_runInfo.m_name.Empty() == false;

				if (hasTag)
				{
					outputString.Append('<');
					outputString.Append(run.m_runInfo.m_name);

					for (const auto& attribute : run.m_runInfo.m_metaData)
					{
						outputString.Append(' ');
						outputString.Append(attribute.Key());
						outputString.Append('=');
						outputString.Append('"');
						outputString.Append(attribute.Value());
						outputString.Append('"');
					}
					outputString.Append('>');
				}

				red::Utf16String runText = run.m_text;
				EscapeText(outputString);
				outputString.Append(runText);

				if ( hasTag )
				{
					outputString.Append( red::Utf16String{ "</>" } );
				}
			}
		}
	}

	void RichTextMarkupWriter::EscapeText( red::Utf16String& textToEscape )
	{
		red::Map< Char, red::Utf16String > escapeCharacters{ PoolText_UTF16() };
		escapeCharacters.Emplace( '&', "&amp;" );
		escapeCharacters.Emplace( '"', "&quot;" );
		escapeCharacters.Emplace( '<', "&lt;" );
		escapeCharacters.Emplace( '>', "&gt;" );

		Int32 escapedStringLength = textToEscape.Length();
		for ( Uint32 textIndex = 0; textIndex < textToEscape.Length(); ++textIndex )
		{
			const Char charToEsc = textToEscape[ textIndex ];
			for ( const auto& escapedChar : escapeCharacters )
			{
				if ( charToEsc == escapedChar.Key() )
				{
					escapedStringLength += ( escapedChar.Value().Length() - 1 );
					break;
				}
			}
		}

		if ( escapedStringLength == textToEscape.Length() )
			return;

		textToEscape.Reserve( escapedStringLength );
		for ( Int32 textIndex = textToEscape.Length() - 1; textIndex >= 0; --textIndex )
		{
			const Char charToEsc = textToEscape[ textIndex ];
			for ( const auto& escapedChar : escapeCharacters )
			{
				if ( charToEsc == escapedChar.Key() )
				{
					textToEscape.Remove( textIndex, textIndex + 1 );
					textToEscape.Append( escapedChar.Value(), textIndex );
					break;
				}
			}
		}
	}

}	// namespace text