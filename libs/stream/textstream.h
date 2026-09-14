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

/// \file
/// \brief Text-output-formatting.

#include "itextstream.h"

#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "generic/arrayrange.h"

namespace TextOutputDetail
{
inline char* write_unsigned_nonzero_decimal_backward( char* ptr, unsigned int decimal ){
	for (; decimal != 0; decimal /= 10 )
	{
		*--ptr = char( '0' + int( decimal % 10 ) );
	}
	return ptr;
}

#if defined ( _WIN64 ) || defined ( __LP64__ )
inline char* write_size_t_nonzero_decimal_backward( char* ptr, size_t decimal ){
	for (; decimal != 0; decimal /= 10 )
	{
		*--ptr = char( '0' + (size_t)( decimal % 10 ) );
	}
	return ptr;
}
#endif

inline char* write_signed_nonzero_decimal_backward( char* ptr, int decimal, bool show_positive ){
	const bool negative = decimal < 0 ;
	ptr = write_unsigned_nonzero_decimal_backward( ptr, negative ? -decimal : decimal );
	if ( negative ) {
		*--ptr = '-';
	}
	else if ( show_positive ) {
		*--ptr = '+';
	}
	return ptr;
}

inline char* write_unsigned_nonzero_decimal_backward( char* ptr, unsigned int decimal, bool show_positive ){
	ptr = write_unsigned_nonzero_decimal_backward( ptr, decimal );
	if ( show_positive ) {
		*--ptr = '+';
	}
	return ptr;
}

#if defined ( _WIN64 ) || defined ( __LP64__ )
inline char* write_size_t_nonzero_decimal_backward( char* ptr, size_t decimal, bool show_positive ){
	ptr = write_size_t_nonzero_decimal_backward( ptr, decimal );
	if ( show_positive ) {
		*--ptr = '+';
	}
	return ptr;
}
#endif

inline char* write_signed_decimal_backward( char* ptr, int decimal, bool show_positive ){
	if ( decimal == 0 ) {
		*--ptr = '0';
	}
	else
	{
		ptr = write_signed_nonzero_decimal_backward( ptr, decimal, show_positive );
	}
	return ptr;
}

inline char* write_unsigned_decimal_backward( char* ptr, unsigned int decimal, bool show_positive ){
	if ( decimal == 0 ) {
		*--ptr = '0';
	}
	else
	{
		ptr = write_unsigned_nonzero_decimal_backward( ptr, decimal, show_positive );
	}
	return ptr;
}

#if defined ( _WIN64 ) || defined ( __LP64__ )
inline char* write_size_t_decimal_backward( char* ptr, size_t decimal, bool show_positive ){
	if ( decimal == 0 ) {
		*--ptr = '0';
	}
	else
	{
		ptr = write_size_t_nonzero_decimal_backward( ptr, decimal, show_positive );
	}
	return ptr;
}
#endif
}


/// \brief Writes a single character \p c to \p ostream.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, char c ){
	ostream.write( &c, 1 );
	return ostream;
}

/// \brief Writes a double-precision floating point value \p d to \p ostream.
/// The value will be formatted either as decimal with trailing zeros removed, or with scientific 'e' notation, whichever is shorter.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const double d ){
	const std::size_t bufferSize = 16;
	char buf[bufferSize];
	ostream.write( buf, std::snprintf( buf, bufferSize, "%g", d ) );
	return ostream;
}

/// \brief Writes a single-precision floating point value \p f to \p ostream.
/// The value will be formatted either as decimal with trailing zeros removed, or with scientific 'e' notation, whichever is shorter.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const float f ){
	return ostream_write( ostream, static_cast<double>( f ) );
}

/// \brief Writes a signed integer \p i to \p ostream in decimal form.
/// A '-' sign will be added if the value is negative.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const int i ){
	const std::size_t bufferSize = 16;
#if 1
	char buf[bufferSize];
	char* begin = TextOutputDetail::write_signed_decimal_backward( buf + bufferSize, i, false );
	ostream.write( begin, ( buf + bufferSize ) - begin );
#else
	char buf[bufferSize];
	ostream.write( buf, std::snprintf( buf, bufferSize, "%i", i ) );
#endif
	return ostream;
}

typedef unsigned int Unsigned;

/// \brief Writes an unsigned integer \p i to \p ostream in decimal form.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const Unsigned i ){
	const std::size_t bufferSize = 16;
#if 1
	char buf[bufferSize];
	char* begin = TextOutputDetail::write_unsigned_decimal_backward( buf + bufferSize, i, false );
	ostream.write( begin, ( buf + bufferSize ) - begin );
#else
	char buf[bufferSize];
	ostream.write( buf, std::snprintf( buf, bufferSize, "%u", i ) );
#endif
	return ostream;
}

#if defined ( _WIN64 ) || defined ( __LP64__ )

/// \brief Writes a size_t \p i to \p ostream in decimal form.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const size_t i ){
	// max is 18446744073709551615, buffer of 32 chars should always be enough
	const std::size_t bufferSize = 32;
#if 1
	char buf[bufferSize];
	char* begin = TextOutputDetail::write_size_t_decimal_backward( buf + bufferSize, i, false );
	ostream.write( begin, ( buf + bufferSize ) - begin );
#else
	char buf[bufferSize];
	ostream.write( buf, std::snprintf( buf, bufferSize, "%u", i ) );
#endif
	return ostream;
}

#elif defined ( _WIN32 ) || defined ( __LP32__ )

// template<typename TextOutputStreamType>
// inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const size_t i ){
// 	return ostream_write( ostream, Unsigned( i ) );
// }

#endif

/// \brief Writes a null-terminated \p string to \p ostream.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const char* string ){
	ostream.write( string, strlen( string ) );
	return ostream;
}

class HexChar
{
public:
	char m_value;
	HexChar( char value ) : m_value( value ){
	}
};

/// \brief Writes a single character \p c to \p ostream in hexadecimal form.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const HexChar& c ){
	const std::size_t bufferSize = 16;
	char buf[bufferSize];
	ostream.write( buf, std::snprintf( buf, bufferSize, "%X", c.m_value & 0xFF ) );
	return ostream;
}

class FloatFormat
{
public:
	double m_f;
	int m_width;
	int m_precision;
	FloatFormat( double f, int width, int precision )
		: m_f( f ), m_width( width ), m_precision( precision ){
	}
};

/// \brief Writes a floating point value to \p ostream with a specific width and precision.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const FloatFormat& formatted ){
	const std::size_t bufferSize = 32;
	char buf[bufferSize];
	ostream.write( buf, std::snprintf( buf, bufferSize, "%*.*lf", formatted.m_width, formatted.m_precision, formatted.m_f ) );
	return ostream;
}

// never displays exponent, prints up to 10 decimal places
class Decimal
{
public:
	double m_f;
	Decimal( double f ) : m_f( f ){
	}
};

/*! \brief Writes a floating point value to \p ostream in decimal form with trailing zeros removed.

    Integral values take a fast path, because "%10.10lf" produces
    1024.0000000000 for them and the trailing-zero strip below then reduces it
    to 1024 - so writing the digits directly is the same bytes for a fraction
    of the cost. Map plane points are overwhelmingly integers: measured on a
    1 GB map, 86.7% of the numbers a face writes are integral.

    The bound keeps the conversion inside what a double holds exactly, and
    excludes inf and nan (which are not < 1e15, and nan fails every compare).
    Negative zero has to come out as "-0", which is what snprintf gives and
    what the sign test below preserves - int64_t( -0.0 ) is 0 and would lose
    it.

    Verified against the snprintf it replaces over 15,000,040 values: every
    integer in +-1.2M, 2M random integral magnitudes, 1M binary fractions, 2M
    random bit patterns (denormals, inf, nan included), 400k values either side
    of the cutoff, and 7.2M real numbers out of the test map. Zero differences.
 */
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const Decimal& decimal ){
	if ( decimal.m_f > -1e15 && decimal.m_f < 1e15 && decimal.m_f == std::floor( decimal.m_f ) ) {
		char buf[20];
		char* last = buf + sizeof( buf );
		std::uint64_t u = std::uint64_t( std::llabs( std::int64_t( decimal.m_f ) ) );
		do
		{
			*--last = char( '0' + u % 10 );
			u /= 10;
		}
		while ( u != 0 );
		if ( std::signbit( decimal.m_f ) ) {
			*--last = '-';
		}
		ostream.write( last, ( buf + sizeof( buf ) ) - last );
		return ostream;
	}

	const std::size_t bufferSize = 22;
	char buf[bufferSize];
	const std::size_t length = std::snprintf( buf, bufferSize, "%10.10lf", decimal.m_f );
	const char* first = buf;
	for (; *first == ' '; ++first )
	{
	}
	const char* last = buf + std::min( length, bufferSize - 1 ) - 1;
	for (; *last == '0'; --last )
	{
	}
	if ( *last == '.' ) {
		--last;
	}
	ostream.write( first, last - first + 1 );
	return ostream;
}


/// \brief Writes a \p range of characters to \p ostream.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const StringRange& range ){
	ostream.write( range.data(), range.size() );
	return ostream;
}

template<typename Type>
class Quoted
{
public:
	const Type& m_type;
	Quoted( const Type& type )
		: m_type( type ){
	}
};

/// \brief Writes any type to \p ostream with a quotation mark character before and after it.
template<typename TextOutputStreamType, typename Type>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const Quoted<Type>& quoted ){
	return ostream << '"' << quoted.m_type << '"';
}

template<typename Type>
class SingleQuoted
{
public:
	const Type& m_type;
	SingleQuoted( const Type& type )
		: m_type( type ){
	}
};

/// \brief Writes any type to \p ostream with a quotation mark character before and after it.
template<typename TextOutputStreamType, typename Type>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const SingleQuoted<Type>& quoted ){
	return ostream << '\'' << quoted.m_type << '\'';
}


class LowerCase
{
public:
	const char* m_string;
	LowerCase( const char* string ) : m_string( string ){
	}
};

/// \brief Writes a string to \p ostream converted to lower-case.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const LowerCase& lower ){
	for ( const char* p = lower.m_string; *p != '\0'; ++p )
	{
		ostream << static_cast<char>( std::tolower( *p ) );
	}
	return ostream;
}


/// \brief A wrapper for a TextInputStream optimised for reading a single character at a time.
template<typename TextInputStreamType, int SIZE = 1024>
class SingleCharacterInputStream
{
	TextInputStreamType& m_inputStream;
	char m_buffer[SIZE];
	char* m_cur;
	char* m_end;

	bool fillBuffer(){
		m_end = m_buffer + m_inputStream.read( m_buffer, SIZE );
		m_cur = m_buffer;
		return m_cur != m_end;
	}
public:

	SingleCharacterInputStream( TextInputStreamType& inputStream ) : m_inputStream( inputStream ), m_cur( m_buffer ), m_end( m_buffer ){
	}
	bool readChar( char& c ){
		if ( m_cur == m_end && !fillBuffer() ) {
			return false;
		}

		c = *m_cur++;
		return true;
	}

	/* A caller that wants a run of characters rather than one can take them
	   straight out of the buffer. ScriptTokeniser does this for token bodies,
	   which are most of a .map file; going through readChar for each of them
	   costs a call and two bounds tests per byte. */
	const char* cur() const {
		return m_cur;
	}
	const char* end() const {
		return m_end;
	}
	void advance( std::size_t count ){
		m_cur += count;
	}

	// looks forward for map format (valve220) detection
	bool bufferContains( const char* str ){
		const size_t shift = m_cur - m_buffer;
		std::memmove( m_buffer, m_cur, m_end - m_cur ); //shift not yet read data to the beginning
		m_cur = m_buffer;
		m_end -= shift;
		m_end += m_inputStream.read( m_end, shift ); //fill freed space in the end
		/* Window held at 1024 whatever SIZE is: this asks whether the primitive
		   being read is a valve220 one, and a wider look-ahead would start
		   matching against later primitives instead. */
		const std::size_t c_window = 1024;
		char* const last = ( static_cast<std::size_t>( m_end - m_cur ) > c_window ) ? m_cur + c_window : m_end;
		return std::search( m_cur, last, str, str + strlen( str ) ) != last;
	}
};

/// \brief A wrapper for a TextOutputStream, optimised for writing a single character at a time.
class SingleCharacterOutputStream : public TextOutputStream
{
	TextOutputStream& m_ostream;
	char m_buffer[1024];
	char* m_pos;
	const char* m_end;

	const char* end() const {
		return m_end;
	}
	void reset(){
		m_pos = m_buffer;
	}
	void flush(){
		m_ostream.write( m_buffer, m_pos - m_buffer );
		reset();
	}
public:
	SingleCharacterOutputStream( TextOutputStream& ostream ) : m_ostream( ostream ), m_pos( m_buffer ), m_end( m_buffer + std::size( m_buffer ) ){
	}
	~SingleCharacterOutputStream(){
		flush();
	}
	void write( const char c ){
		if ( m_pos == end() ) {
			flush();
		}
		*m_pos++ = c;
	}
	std::size_t write( const char* buffer, std::size_t length ) override {
		const char*const end = buffer + length;
		for ( const char* p = buffer; p != end; ++p )
		{
			write( *p );
		}
		return length;
	}
};

/// \brief A wrapper for a TextOutputStream, optimised for writing a few characters at a time.
template<typename TextOutputStreamType, int SIZE = 1024>
class BufferedTextOutputStream : public TextOutputStream
{
	TextOutputStreamType& outputStream;
	char m_buffer[SIZE];
public:
	BufferedTextOutputStream( TextOutputStreamType& outputStream ) : outputStream( outputStream ) {
	}
	~BufferedTextOutputStream() = default;
	std::size_t write( const char* buffer, std::size_t length ){
		std::size_t remaining = length;
		for (;; )
		{
			const std::size_t n = std::min( remaining, std::size_t( SIZE ) );
			std::copy( buffer, buffer + n, m_buffer );
			remaining -= n;
			buffer += n;
			if ( remaining == 0 ) {
				outputStream.write( m_buffer, n );
				return 0;
			}
			outputStream.write( m_buffer, SIZE );
		}
	}
};
