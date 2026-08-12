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
#include "stream/textstream.h"

#include <cstring>

/*! \brief Writes tokens to a text-output-stream, separated by spaces and newlines.

    Buffered, and deliberately not through TextOutputStream. Writing the
    separator was a call of its own before the token's own call, so a map cost
    two virtual calls a token - 420 million of them on the 1 GB test map, each
    crossing into another module. The write() below is not virtual and not an
    override, so ostream_write() binds to it directly and formatting a
    coordinate compiles into a memcpy into m_buffer. The stream sees one call
    per 64 KB.
 */
class SimpleTokenWriter final : public TokenWriter
{
public:
	SimpleTokenWriter( TextOutputStream& ostream )
		: m_ostream( ostream ), m_separator( '\n' ){
	}
	~SimpleTokenWriter(){
		writeSeparator();
		flush();
	}
	void release() override {
		delete this;
	}
	void nextLine() override {
		m_separator = '\n';
	}
	void writeToken( const char* token ) override {
		ASSERT_MESSAGE( strchr( token, ' ' ) == 0, "token contains whitespace: " );
		writeSeparator();
		ostream_write( *this, token );
	}
	void writeString( const char* string ) override {
		writeSeparator();
		ostream_write( *this, '"' );
		ostream_write( *this, string );
		ostream_write( *this, '"' );
	}
	void writeInteger( int i ) override {
		writeSeparator();
		ostream_write( *this, i );
	}
	void writeUnsigned( std::size_t i ) override {
		writeSeparator();
		ostream_write( *this, i );
	}
	void writeFloat( double f ) override {
		writeSeparator();
		ostream_write( *this, Decimal( f ) );
	}

	/// \brief Appends \p length characters to the buffer, flushing it first if
	/// they will not fit. Anything longer than the whole buffer goes straight
	/// to the stream, after the buffer is flushed so the order is kept.
	std::size_t write( const char* buffer, std::size_t length ){
		if ( m_pos + length > c_bufferSize ) {
			flush();
			if ( length > c_bufferSize ) {
				return m_ostream.write( buffer, length );
			}
		}
		std::memcpy( m_buffer + m_pos, buffer, length );
		m_pos += length;
		return length;
	}

private:
	void writeSeparator(){
		if ( m_pos == c_bufferSize ) {
			flush();
		}
		m_buffer[m_pos++] = m_separator;
		m_separator = ' ';
	}
	void flush(){
		if ( m_pos != 0 ) {
			m_ostream.write( m_buffer, m_pos );
			m_pos = 0;
		}
	}
	static const std::size_t c_bufferSize = 64 * 1024;
	TextOutputStream& m_ostream;
	char m_separator;
	std::size_t m_pos = 0;
	char m_buffer[c_bufferSize];
};

inline TokenWriter& NewSimpleTokenWriter( TextOutputStream& ostream ){
	return *( new SimpleTokenWriter( ostream ) );
}
