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

#include "itextstream.h"
#include "writestats.h"
#include <cstdio>
#include <cstring>

/// \brief A wrapper around a file input stream opened for reading in text mode. Similar to std::ifstream.
class TextFileInputStream : public TextInputStream
{
	FILE* m_file;
public:
	TextFileInputStream( const char* name ){
		m_file = name[0] == '\0' ? 0 : fopen( name, "rt" );
	}
	~TextFileInputStream(){
		if ( !failed() ) {
			fclose( m_file );
		}
	}

	bool failed() const {
		return m_file == 0;
	}

	std::size_t read( char* buffer, std::size_t length ) override {
		return fread( buffer, 1, length, m_file );
	}
};

/// \brief A wrapper around a file input stream opened for writing in text mode. Similar to std::ofstream.
/*! \brief A file opened for writing text.

    Buffered internally rather than leaning on stdio's: the map writer calls
    write() once for the separator before every token and again for the token
    itself, which on a 6.8M face map is upwards of 400 million calls, each
    paying stdio's lock and - because the file is opened in text mode - a scan
    for newlines to translate. Gathering them here first turns that into one
    fwrite per 64 KB.
 */
class TextFileOutputStream : public TextOutputStream
{
	FILE* m_file;
	bool m_failed;
	static const std::size_t c_bufferSize = 64 * 1024;
	char m_buffer[c_bufferSize];
	std::size_t m_pos = 0;

	void flush(){
		if ( m_pos != 0 ) {
			write_counted( m_buffer, m_pos );
			m_pos = 0;
		}
	}
	std::size_t write_counted( const char* buffer, std::size_t length ){
		WriteTimerScope ioTime( g_writeStats.ioSeconds );
		++g_writeStats.fwrites;
		g_writeStats.bytes += length;
		const std::size_t written = fwrite( buffer, 1, length, m_file );
		if ( written != length ) {
			m_failed = true;
		}
		return written;
	}
public:
	TextFileOutputStream( const char* name )
		: m_file( name[0] == '\0' ? 0 : fopen( name, "wt" ) ),
		  m_failed( m_file == 0 ){
	}
	~TextFileOutputStream(){
		close();
	}

	/*! \brief Flushes and closes the stream, reporting failures which can occur
	    long after open() succeeded (most importantly, a full disk).

	    fclose() is part of the result because stdio may still hold translated
	    text-mode bytes after our own 64 KB buffer has been flushed. */
	bool close(){
		if ( m_file != 0 ) {
			flush();
			FILE* file = m_file;
			m_file = 0;
			if ( fclose( file ) != 0 ) {
				m_failed = true;
			}
		}
		return !m_failed;
	}

	bool failed() const {
		return m_failed;
	}

	std::size_t write( const char* buffer, std::size_t length ) override {
		++g_writeStats.writeCalls;
		if ( m_failed || m_file == 0 ) {
			return 0;
		}
		if ( length >= c_bufferSize ) { // too big to be worth gathering
			flush();
			if ( m_failed ) {
				return 0;
			}
			return write_counted( buffer, length );
		}
		if ( m_pos + length > c_bufferSize ) {
			flush();
			if ( m_failed ) {
				return 0;
			}
		}
		std::memcpy( m_buffer + m_pos, buffer, length );
		m_pos += length;
		return length;
	}
};

template<typename T>
inline TextFileOutputStream& operator<<( TextFileOutputStream& ostream, const T& t ){
	return ostream_write( ostream, t );
}
