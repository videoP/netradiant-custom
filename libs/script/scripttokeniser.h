/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "iscriplib.h"

#include <cstring>

/*! \brief Whether tokenise() may take the fast path below.

    Defined in radiant/parse.cpp, which is the only place a ScriptTokeniser is
    ever constructed, and set from the Large Maps preferences. Off restores the
    stock state machine byte for byte, so a map that reads oddly can be checked
    against it without a rebuild.
 */
extern bool g_scriptTokeniser_fastPath;

class ScriptTokeniser final : public Tokeniser
{
	enum CharType
	{
		eWhitespace,
		eCharToken,
		eNewline,
		eCharQuote,
		eCharSolidus,
		eCharStar,
		eCharSpecial,
	};

	typedef bool ( ScriptTokeniser::*Tokenise )( char c );

	/* 64k rather than 1k: a 1 GB map is a million refills at the smaller size,
	   and the fast path below consumes straight out of this buffer, so a bigger
	   one also means fewer trips back through readChar. */
	static const int c_bufferSize = 64 * 1024;

	Tokenise m_stack[3];
	Tokenise* m_state;
	SingleCharacterInputStream<TextInputStream, c_bufferSize> m_istream;
	std::size_t m_scriptline;
	std::size_t m_scriptcolumn;

	char m_token[MAXTOKEN];
	char* m_write;

	char m_current;
	bool m_eof;
	bool m_crossline;
	bool m_unget;
	bool m_emit;

	const bool m_special;
	const bool m_specialComments;
	const char m_specialCommentSig[4] = "@$&";
	const char *m_specialCommentRead;

	/* charType() was a switch per character, and the state machine asks it for
	   every byte of the input. m_special is fixed for the life of the tokeniser,
	   so the whole answer can be tabulated once. m_tokenBody records the same
	   table's "this character continues a token" answer, which is the test the
	   fast path runs per byte. */
	unsigned char m_charType[256];
	bool m_tokenBody[256];

	static CharType charTypeOf( const char c, const bool special ){
		switch ( c )
		{
		case '\n':
			return eNewline;
		case '"':
			return eCharQuote;
		case '/':
			return eCharSolidus;
		case '*':
			return eCharStar;
		case '{':
		case '(':
		case '}':
		case ')':
		case '[':
		case ']':
		case ',':
		case ':':
			return ( special ) ? eCharSpecial : eCharToken;
		}

		if ( c > 32 ) {
			return eCharToken;
		}
		return eWhitespace;
	}

	void buildCharTypes(){
		for ( int i = 0; i < 256; ++i )
		{
			const CharType type = charTypeOf( static_cast<char>( i ), m_special );
			m_charType[i] = static_cast<unsigned char>( type );
			/* Exactly the set tokeniseToken() adds rather than emits on. Solidus
			   is in it because MID_TOKEN_COMMENTS is off, so a '/' mid-token
			   falls through to the add cases. */
			m_tokenBody[i] = ( type == eCharToken || type == eCharStar || type == eCharSolidus );
		}
	}

	CharType charType( const char c ){
		return static_cast<CharType>( m_charType[ static_cast<unsigned char>( c ) ] );
	}
	bool isTokenBody( const char c ) const {
		return m_tokenBody[ static_cast<unsigned char>( c ) ];
	}

	Tokenise state(){
		return *m_state;
	}
	void push( Tokenise state ){
		ASSERT_MESSAGE( m_state != m_stack + 2, "token parser: illegal stack push" );
		*( ++m_state ) = state;
	}
	void pop(){
		ASSERT_MESSAGE( m_state != m_stack, "token parser: illegal stack pop" );
		--m_state;
	}
	void add( const char c ){
		if ( m_write < m_token + MAXTOKEN - 1 ) {
			*m_write++ = c;
		}
	}
	/// \brief add() over a run. Overflow is truncated, exactly as add() drops it.
	void append( const char* first, const char* last ){
		std::size_t count = last - first;
		const std::size_t room = ( m_token + MAXTOKEN - 1 ) - m_write;
		if ( count > room ) {
			count = room;
		}
		std::memcpy( m_write, first, count );
		m_write += count;
	}
	void remove(){
		ASSERT_MESSAGE( m_write > m_token, "no char to remove" );
		--m_write;
	}

	bool tokeniseDefault( char c ){
		switch ( charType( c ) )
		{
		case eNewline:
			if ( !m_crossline ) {
				globalErrorStream() << getLine() << ':' << getColumn() << ": unexpected end-of-line before token\n";
				return false;
			}
			break;
		case eCharToken:
		case eCharStar:
			push( Tokenise( &ScriptTokeniser::tokeniseToken ) );
			add( c );
			break;
		case eCharSpecial:
			push( Tokenise( &ScriptTokeniser::tokeniseSpecial ) );
			add( c );
			break;
		case eCharQuote:
			push( Tokenise( &ScriptTokeniser::tokeniseQuotedToken ) );
			break;
		case eCharSolidus:
			push( Tokenise( &ScriptTokeniser::tokeniseSolidus ) );
			break;
		default:
			break;
		}
		return true;
	}
	bool tokeniseToken( char c ){
		switch ( charType( c ) )
		{
		case eNewline:
		case eWhitespace:
		case eCharQuote:
		case eCharSpecial:
			pop();
			m_emit = true; // emit token
			break;
		case eCharSolidus:
#define MID_TOKEN_COMMENTS 0
#if MID_TOKEN_COMMENTS //SPoG: ignore comments in the middle of tokens.
			push( Tokenise( &ScriptTokeniser::tokeniseSolidus ) );
			break;
#endif
		case eCharToken:
		case eCharStar:
			add( c );
			break;
		default:
			break;
		}
		return true;
	}
	bool tokeniseQuotedToken( char c ){
		switch ( charType( c ) )
		{
		case eNewline:
			if ( m_crossline ) {
				globalErrorStream() << getLine() << ':' << getColumn() << ": unexpected end-of-line in quoted token\n";
				return false;
			}
			break;
		case eWhitespace:
		case eCharToken:
		case eCharSolidus:
		case eCharStar:
		case eCharSpecial:
			add( c );
			break;
		case eCharQuote:
			pop();
			push( Tokenise( &ScriptTokeniser::tokeniseEndQuote ) );
			break;
		default:
			break;
		}
		return true;
	}
	bool tokeniseSolidus( char c ){
		switch ( charType( c ) )
		{
		case eNewline:
		case eWhitespace:
		case eCharQuote:
		case eCharSpecial:
			pop();
			add( '/' );
			m_emit = true; // emit single slash
			break;
		case eCharToken:
			pop();
			add( '/' );
			add( c );
			break;
		case eCharSolidus:
			pop();
			if( m_specialComments ){
				push( Tokenise( &ScriptTokeniser::tokeniseCommentSpecialSig ) );
				m_specialCommentRead = m_specialCommentSig;
			}
			else
				push( Tokenise( &ScriptTokeniser::tokeniseComment ) );
			break; // dont emit single slash
		case eCharStar:
			pop();
			push( Tokenise( &ScriptTokeniser::tokeniseBlockComment ) );
			break; // dont emit single slash
		default:
			break;
		}
		return true;
	}
	bool tokeniseComment( char c ){
		if ( c == '\n' ) {
			pop();
#if MID_TOKEN_COMMENTS
			if ( state() == Tokenise( &ScriptTokeniser::tokeniseToken ) ) {
				pop();
				m_emit = true; // emit token immediately preceding comment
			}
#endif
		}
		return true;
	}
	bool tokeniseCommentSpecialSig( char c ){ // test for '//@$&' signature
		if( *m_specialCommentRead++ == c ){
			if( *m_specialCommentRead == '\0' ) // match, do tokeniseDefault
				pop(); // note no MID_TOKEN_COMMENTS support
			return true;
		}
		pop();
		push( Tokenise( &ScriptTokeniser::tokeniseComment ) );
		return tokeniseComment( c );
	}
	bool tokeniseBlockComment( char c ){
		if ( c == '*' ) {
			pop();
			push( Tokenise( &ScriptTokeniser::tokeniseEndBlockComment ) );
		}
		return true;
	}
	bool tokeniseEndBlockComment( char c ){
		switch ( c )
		{
		case '/':
			pop();
#if MID_TOKEN_COMMENTS
			if ( state() == Tokenise( &ScriptTokeniser::tokeniseToken ) ) {
				pop();
				m_emit = true; // emit token immediately preceding comment
			}
#endif
			break; // dont emit comment
		case '*':
			break; // no state change
		default:
			pop();
			push( Tokenise( &ScriptTokeniser::tokeniseBlockComment ) );
			break;
		}
		return true;
	}
	bool tokeniseEndQuote( char c ){
		pop();
		m_emit = true; // emit quoted token
		return true;
	}
	bool tokeniseSpecial( char c ){
		pop();
		m_emit = true; // emit single-character token
		return true;
	}

	/*! \brief The common case, taken straight out of the input buffer.

	    Nearly every byte of a .map file is either a separator or part of an
	    ordinary token, and recognising that through the state machine costs an
	    indirect call through a pointer-to-member, a switch, and a readChar per
	    byte. Measured on a 1 GB map: tokenising ran at ~44 MB/s.

	    Only the cases whose behaviour is obvious are handled here. Quotes,
	    comments, special characters and a newline where one is not allowed all
	    return eDefer, and the state machine deals with them exactly as before -
	    so the error messages, line/column numbers and token contents are
	    unchanged, and the awkward parts of the grammar have no second
	    implementation to keep in step.

	    Returns eToken if m_token now holds one, eEnd at end of input.
	 */
	enum FastResult { eDefer, eToken, eEnd };

	FastResult tokeniseFast(){
#if MID_TOKEN_COMMENTS
		return eDefer; // m_tokenBody assumes a mid-token '/' just extends the token
#else
		// separators
		for (;; )
		{
			if ( m_eof ) {
				return eEnd;
			}
			const char c = m_current;
			if ( c == '\n' ) {
				if ( !m_crossline ) {
					return eDefer; // the state machine reports this
				}
				++m_scriptline;
				m_scriptcolumn = 1;
			}
			else if ( charType( c ) == eWhitespace ) {
				++m_scriptcolumn;
			}
			else if ( charType( c ) == eCharToken ) {
				break; // start of an ordinary token
			}
			else{
				return eDefer; // quote, solidus, star, special
			}
			m_eof = !m_istream.readChar( m_current );
		}

		add( m_current );
		++m_scriptcolumn;

		// body
		for (;; )
		{
			const char* const first = m_istream.cur();
			const char* const last = m_istream.end();
			const char* p = first;
			while ( p != last && isTokenBody( *p ) )
			{
				++p;
			}

			append( first, p );
			m_scriptcolumn += p - first;
			m_istream.advance( p - first );

			if ( !m_istream.readChar( m_current ) ) {
				m_eof = true;   // the state machine also emits what it has at eof
				return eToken;
			}
			if ( p != last ) {
				/* A terminator, left unconsumed and uncounted - the state
				   machine emits before advancing past it too, so the next call
				   sees it as the first character in the default state. */
				return eToken;
			}
			// ran out of buffer mid-token; readChar refilled it
			if ( !isTokenBody( m_current ) ) {
				return eToken;
			}
			add( m_current );
			++m_scriptcolumn;
		}
#endif
	}

	/// Returns true if a token was successfully parsed.
	bool tokenise(){
		m_write = m_token;

		if ( g_scriptTokeniser_fastPath && m_state == m_stack ) { // nothing pushed: the fast path's assumption
			switch ( tokeniseFast() )
			{
			case eToken:
				return true;
			case eEnd:
				return m_write != m_token;
			case eDefer:
				break;
			}
		}

		while ( !eof() )
		{
			char c = m_current;

			if ( !( ( *this ).*state() )( c ) ) {
				// parse error
				m_eof = true;
				return false;
			}
			if ( m_emit ) {
				m_emit = false;
				return true;
			}

			if ( c == '\n' ) {
				++m_scriptline;
				m_scriptcolumn = 1;
			}
			else
			{
				++m_scriptcolumn;
			}

			m_eof = !m_istream.readChar( m_current );
		}
		return m_write != m_token;
	}

	const char* fillToken(){
		if ( !tokenise() ) {
			return 0;
		}

		add( '\0' );
		return m_token;
	}

	bool eof(){
		return m_eof;
	}

public:
	ScriptTokeniser( TextInputStream& istream, bool special, bool specialComments ) :
		m_state( m_stack ),
		m_istream( istream ),
		m_scriptline( 1 ),
		m_scriptcolumn( 1 ),
		m_crossline( false ),
		m_unget( false ),
		m_emit( false ),
		m_special( special ),
		m_specialComments( specialComments ){
		buildCharTypes();
		m_stack[0] = Tokenise( &ScriptTokeniser::tokeniseDefault );
		m_eof = !m_istream.readChar( m_current );
		m_token[MAXTOKEN - 1] = '\0';
	}
	void release() override {
		delete this;
	}
	void nextLine() override {
		m_crossline = true;
	}
	const char* getToken() override {
		if ( m_unget ) {
			m_unget = false;
			return m_token;
		}

		return fillToken();
	}
	void ungetToken() override {
		ASSERT_MESSAGE( !m_unget, "can't unget more than one token" );
		m_unget = true;
	}
	std::size_t getLine() const override {
		return m_scriptline;
	}
	std::size_t getColumn() const override {
		return m_scriptcolumn;
	}
	bool bufferContains( const char* str ) override {
		return m_istream.bufferContains( str );
	}
};


inline Tokeniser& NewScriptTokeniser( TextInputStream& istream ){
	return *( new ScriptTokeniser( istream, true, false ) );
}

inline Tokeniser& NewMapTokeniser( TextInputStream& istream ){
	return *( new ScriptTokeniser( istream, false, true ) );
}

inline Tokeniser& NewSimpleTokeniser( TextInputStream& istream ){
	return *( new ScriptTokeniser( istream, false, false ) );
}
