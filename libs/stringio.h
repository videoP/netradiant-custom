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

#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <limits>

#include "generic/vector.h"
#include "iscriplib.h"
#include "string/string.h"
#include "generic/callback.h"

inline float string_read_float( const char* string ){
	return atof( string );
}

inline int string_read_int( const char* string ){
	return atoi( string );
}

inline bool char_is_whitespace( char c ){
	return c == ' ' || c == '\t';
}

inline const char* string_remove_whitespace( const char* string ){
	for (;; )
	{
		if ( !char_is_whitespace( *string ) ) {
			break;
		}
		++string;
	}
	return string;
}

inline const char* string_remove_zeros( const char* string ){
	for (;; )
	{
		char c = *string;
		if ( c != '0' ) {
			break;
		}
		++string;
	}
	return string;
}

inline const char* string_remove_sign( const char* string ){
	if ( *string == '-' || *string == '+' ) { // signed zero - acceptable
		return ++string;
	}
	return string;
}

inline bool string_is_unsigned_zero( const char* string ){
	for (; *string != '\0'; ++string )
	{
		if ( *string != '0' ) {
			return false;
		}
	}
	return true;
}

inline bool string_is_signed_zero( const char* string ){
	return string_is_unsigned_zero( string_remove_sign( string ) );
}

//[whitespaces][+|-][nnnnn][.nnnnn][e|E[+|-]nnnn]
//(where whitespaces are any tab or space character and nnnnn may be any number of digits)
inline bool string_is_float_zero( const char* string ){
	string = string_remove_whitespace( string );
	if ( string_empty( string ) ) {
		return false;
	}

	string = string_remove_sign( string );
	if ( string_empty( string ) ) {
		// no whole number or fraction part
		return false;
	}

	// whole-number part
	string = string_remove_zeros( string );
	if ( string_empty( string ) ) {
		// no fraction or exponent
		return true;
	}
	if ( *string == '.' ) {
		// fraction part
		if ( *string++ != '0' ) {
			// invalid fraction
			return false;
		}
		string = string_remove_zeros( ++string );
		if ( string_empty( string ) ) {
			// no exponent
			return true;
		}
	}
	if ( *string == 'e' || *string == 'E' ) {
		// exponent part
		string = string_remove_sign( ++string );
		if ( *string++ != '0' ) {
			// invalid exponent
			return false;
		}
		string = string_remove_zeros( ++string );
		if ( string_empty( string ) ) {
			// no trailing whitespace
			return true;
		}
	}
	string = string_remove_whitespace( string );
	return string_empty( string );
}

/*! \brief Whether to take the short-cut in buffer_parse_floating_literal and
    buffer_parse_signed_decimal_integer_literal below.

    Compile-time rather than a Large Maps checkbox, deliberately: this header is
    compiled into the module DLLs (entity, mapq3, shaders, md3model) as well as
    radiant.exe, so a runtime switch would have to be reachable across the
    module boundary and that means adding to the module ABI for a diagnostic.
    Set to 0 and rebuild to fall back to strtod/strtol everywhere.
 */
#define RADIANT_FAST_NUMBER_PARSE 1

/*! \brief strtod, short-cut for the shape of number a .map file is made of.

    strtod is locale-aware and completely general, and measures at ~96ns a call
    here. A brush face costs eighteen of them - nine plane coordinates, six
    texdef values, three flags - so a 6.8M face map spends about twelve seconds
    inside it, which was most of the time attributed to "tokenise".

    This reads [+-]digits[.digits][(e|E)[+-]digits] directly and hands anything
    else - leading whitespace, hex, inf, nan, more precision than the shortcut
    can hold - straight to strtod, from the original position, so those keep
    their existing behaviour exactly.

    The shortcut is exact rather than approximate. A mantissa of 2^53 or less is
    held exactly by a double, and so is 10^n for |n| <= 22; one IEEE multiply or
    divide of two exact values is correctly rounded, which is the same value
    strtod is required to produce. Outside either bound it does not try. Every
    coordinate and texdef in a .map is well inside both.
 */
inline double buffer_parse_floating_literal( const char*& buffer ){
#if !RADIANT_FAST_NUMBER_PARSE
	return strtod( buffer, const_cast<char**>( &buffer ) );
#else
	const char* p = buffer;

	bool negative = false;
	if ( *p == '-' ) {
		negative = true;
		++p;
	}
	else if ( *p == '+' ) {
		++p;
	}

	if ( p[0] == '0' && ( p[1] == 'x' || p[1] == 'X' ) ) {
		return strtod( buffer, const_cast<char**>( &buffer ) ); // hex float literal
	}

	std::uint64_t mantissa = 0;
	int significant = 0;  // digits accumulated into mantissa, leading zeros excluded
	int exponent = 0;     // power of ten still to apply
	bool anyDigits = false;

	for ( ; *p >= '0' && *p <= '9'; ++p )
	{
		anyDigits = true;
		if ( mantissa != 0 || *p != '0' ) {
			if ( ++significant > 19 ) {
				return strtod( buffer, const_cast<char**>( &buffer ) );
			}
			mantissa = mantissa * 10 + static_cast<unsigned>( *p - '0' );
		}
	}

	if ( *p == '.' ) {
		++p;
		for ( ; *p >= '0' && *p <= '9'; ++p )
		{
			anyDigits = true;
			--exponent;
			if ( mantissa != 0 || *p != '0' ) {
				if ( ++significant > 19 ) {
					return strtod( buffer, const_cast<char**>( &buffer ) );
				}
				mantissa = mantissa * 10 + static_cast<unsigned>( *p - '0' );
			}
		}
	}

	if ( !anyDigits ) {
		return strtod( buffer, const_cast<char**>( &buffer ) ); // inf, nan, hex, "." , whitespace
	}

	if ( *p == 'e' || *p == 'E' ) {
		const char* q = p + 1;
		int exponentSign = 1;
		if ( *q == '-' ) {
			exponentSign = -1;
			++q;
		}
		else if ( *q == '+' ) {
			++q;
		}
		if ( *q >= '0' && *q <= '9' ) {
			int value = 0;
			for ( ; *q >= '0' && *q <= '9'; ++q )
			{
				if ( value < 1000 ) { // no need for more; it is out of range either way
					value = value * 10 + ( *q - '0' );
				}
			}
			exponent += exponentSign * value;
			p = q;
		}
		/* else: not a valid exponent, so the number ends before the 'e' - which
		   is what strtod does too. */
	}

	if ( exponent < -22 || exponent > 22 || mantissa > ( std::uint64_t( 1 ) << 53 ) ) {
		return strtod( buffer, const_cast<char**>( &buffer ) );
	}

	static const double c_pow10[23] = {
		1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
		1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
	};

	double value = static_cast<double>( mantissa );
	value = ( exponent >= 0 ) ? value * c_pow10[exponent] : value / c_pow10[-exponent];

	buffer = p;
	return negative ? -value : value;
#endif
}

/// \brief strtol, short-cut the same way. See buffer_parse_floating_literal.
inline int buffer_parse_signed_decimal_integer_literal( const char*& buffer ){
#if !RADIANT_FAST_NUMBER_PARSE
	return strtol( buffer, const_cast<char**>( &buffer ), 10 );
#else
	const char* p = buffer;

	bool negative = false;
	if ( *p == '-' ) {
		negative = true;
		++p;
	}
	else if ( *p == '+' ) {
		++p;
	}

	if ( !( *p >= '0' && *p <= '9' ) ) {
		return strtol( buffer, const_cast<char**>( &buffer ), 10 );
	}

	std::uint64_t value = 0;
	for ( ; *p >= '0' && *p <= '9'; ++p )
	{
		value = value * 10 + static_cast<unsigned>( *p - '0' );
		if ( value > 0x80000000u ) { // overflow, which strtol saturates and errno-flags
			return strtol( buffer, const_cast<char**>( &buffer ), 10 );
		}
	}
	if ( value > ( negative ? 0x80000000u : 0x7fffffffu ) ) {
		return strtol( buffer, const_cast<char**>( &buffer ), 10 );
	}

	buffer = p;
	if ( negative ) {
		/* Converting 0x80000000 to int commonly produces INT_MIN, whose unary
		   negation is signed overflow. Construct that one result explicitly. */
		return value == 0x80000000u
		     ? std::numeric_limits<int>::min()
		     : -static_cast<int>( value );
	}
	return static_cast<int>( value );
#endif
}

inline int buffer_parse_unsigned_decimal_integer_literal( const char*& buffer ){
	return strtoul( buffer, const_cast<char**>( &buffer ), 10 );
}

// [+|-][nnnnn][.nnnnn][e|E[+|-]nnnnn]
inline bool string_parse_float( const char* string, float& f ){
	if ( string_empty( string ) ) {
		return false;
	}
	f = float( buffer_parse_floating_literal( string ) );
	return string_empty( string );
}

// format same as float
inline bool string_parse_double( const char* string, double& f ){
	if ( string_empty( string ) ) {
		return false;
	}
	f = buffer_parse_floating_literal( string );
	return string_empty( string );
}

// <float><space><float><space><float>
template<typename Element>
inline bool string_parse_vector3( const char* string, BasicVector3<Element>& v ){
	if ( string_empty( string ) || *string == ' ' ) {
		return false;
	}
	v[0] = float( buffer_parse_floating_literal( string ) );
	if ( *string++ != ' ' ) {
		return false;
	}
	v[1] = float( buffer_parse_floating_literal( string ) );
	if ( *string++ != ' ' ) {
		return false;
	}
	v[2] = float( buffer_parse_floating_literal( string ) );
	return string_empty( string );
}

template<typename Float>
inline bool string_parse_vector( const char* string, Float* first, Float* last ){
	if ( first != last && ( string_empty( string ) || *string == ' ' ) ) {
		return false;
	}
	for (;; )
	{
		*first = float( buffer_parse_floating_literal( string ) );
		if ( ++first == last ) {
			return string_empty( string );
		}
		if ( *string++ != ' ' ) {
			return false;
		}
	}
}

// decimal signed integer
inline bool string_parse_int( const char* string, int& i ){
	if ( string_empty( string ) ) {
		return false;
	}
	i = buffer_parse_signed_decimal_integer_literal( string );
	return string_empty( string );
}

// decimal unsigned integer
inline bool string_parse_size( const char* string, std::size_t& i ){
	if ( string_empty( string ) ) {
		return false;
	}
	i = buffer_parse_unsigned_decimal_integer_literal( string );
	return string_empty( string );
}


//#define RETURN_FALSE_IF_FAIL( expression ) if ( !expression ) {return false; }else
#define RETURN_FALSE_IF_FAIL( expression ) do{ if ( !expression ) { return false; } }while( false )

inline void Tokeniser_unexpectedError( Tokeniser& tokeniser, const char* token, const char* expected ){
	globalErrorStream() << tokeniser.getLine() << ':' << tokeniser.getColumn() << ": parse error at " << SingleQuoted( token != 0 ? token : "#EOF" ) << ": expected " << SingleQuoted( expected ) << '\n';
}


inline bool Tokeniser_getFloat( Tokeniser& tokeniser, float& f ){
	const char* token = tokeniser.getToken();
	if ( token != 0 && string_parse_float( token, f ) ) {
		return true;
	}
	//fallback for 1.#IND 1.#INF 1.#QNAN cases, happening sometimes after texture locking algorithms
	else if ( token != 0 && strstr( token, ".#" ) ) {
		globalWarningStream() << "Warning: " << tokeniser.getLine() << ':' << tokeniser.getColumn() << ": expected parse problem at " << SingleQuoted( token ) << ": wanted '#number'\nProcessing anyway\n";
//		*strstr( token, ".#" ) = '\0';
		return true;
	}
	Tokeniser_unexpectedError( tokeniser, token, "#number" );
	return false;
}

inline bool Tokeniser_getDouble( Tokeniser& tokeniser, double& f ){
	const char* token = tokeniser.getToken();
	if ( token != 0 && string_parse_double( token, f ) ) {
		return true;
	}
	Tokeniser_unexpectedError( tokeniser, token, "#number" );
	return false;
}

inline bool Tokeniser_getInteger( Tokeniser& tokeniser, int& i ){
	const char* token = tokeniser.getToken();
	if ( token != 0 && string_parse_int( token, i ) ) {
		return true;
	}
	Tokeniser_unexpectedError( tokeniser, token, "#integer" );
	return false;
}

inline bool Tokeniser_getSize( Tokeniser& tokeniser, std::size_t& i ){
	const char* token = tokeniser.getToken();
	if ( token != 0 && string_parse_size( token, i ) ) {
		return true;
	}
	Tokeniser_unexpectedError( tokeniser, token, "#unsigned-integer" );
	return false;
}

inline bool Tokeniser_parseToken( Tokeniser& tokeniser, const char* expected ){
	const char* token = tokeniser.getToken();
	if ( token != 0 && string_equal( token, expected ) ) {
		return true;
	}
	Tokeniser_unexpectedError( tokeniser, token, expected );
	return false;
}

inline bool Tokeniser_nextTokenIsDigit( Tokeniser& tokeniser ){
	const char* token = tokeniser.getToken();
	if ( token == 0 ) {
		return false;
	}
	char c = *token;
	tokeniser.ungetToken();
	return std::isdigit( c ) != 0;
}

inline bool Tokeniser_inlineTokenAvailable( Tokeniser& tokeniser ){
	const size_t line = tokeniser.getLine();
	if ( tokeniser.getToken() ) {
		tokeniser.ungetToken();
		return line == tokeniser.getLine();
	}
	return false;
}

inline void Tokeniser_skipToNextLine( Tokeniser& tokeniser ){
	const size_t line = tokeniser.getLine();
	while ( tokeniser.getToken() && line == tokeniser.getLine() ) {}
	tokeniser.ungetToken();
}




inline void CopiedString_importString( CopiedString& self, const char* string ){
	self = string;
}
typedef ReferenceCaller<CopiedString, void(const char*), CopiedString_importString> CopiedStringImportStringCaller;
inline void CopiedString_exportString( const CopiedString& self, const StringImportCallback& importer ){
	importer( self.c_str() );
}
typedef ConstReferenceCaller<CopiedString, void(const StringImportCallback&), CopiedString_exportString> CopiedStringExportStringCaller;

inline void Bool_importString( bool& self, const char* string ){
	self = string_equal( string, "true" );
}
typedef ReferenceCaller<bool, void(const char*), Bool_importString> BoolImportStringCaller;
inline void Bool_exportString( const bool& self, const StringImportCallback& importer ){
	importer( self ? "true" : "false" );
}
typedef ConstReferenceCaller<bool, void(const StringImportCallback&), Bool_exportString> BoolExportStringCaller;

inline void Int_importString( int& self, const char* string ){
	if ( !string_parse_int( string, self ) ) {
		self = 0;
	}
}
typedef ReferenceCaller<int, void(const char*), Int_importString> IntImportStringCaller;
inline void Int_exportString( const int& self, const StringImportCallback& importer ){
	char buffer[16];
	sprintf( buffer, "%d", self );
	importer( buffer );
}
typedef ConstReferenceCaller<int, void(const StringImportCallback&), Int_exportString> IntExportStringCaller;

inline void Size_importString( std::size_t& self, const char* string ){
	int i;
	if ( string_parse_int( string, i ) && i >= 0 ) {
		self = i;
	}
	else
	{
		self = 0;
	}
}
typedef ReferenceCaller<std::size_t, void(const char*), Size_importString> SizeImportStringCaller;
inline void Size_exportString( const std::size_t& self, const StringImportCallback& importer ){
	char buffer[16];
	sprintf( buffer, "%u", Unsigned( self ) );
	importer( buffer );
}
typedef ConstReferenceCaller<std::size_t, void(const StringImportCallback&), Size_exportString> SizeExportStringCaller;

inline void Float_importString( float& self, const char* string ){
	if ( !string_parse_float( string, self ) ) {
		self = 0;
	}
}
typedef ReferenceCaller<float, void(const char*), Float_importString> FloatImportStringCaller;
inline void Float_exportString( const float& self, const StringImportCallback& importer ){
	char buffer[16];
	sprintf( buffer, "%g", self );
	importer( buffer );
}
typedef ConstReferenceCaller<float, void(const StringImportCallback&), Float_exportString> FloatExportStringCaller;

inline void Vector3_importString( Vector3& self, const char* string ){
	if ( !string_parse_vector3( string, self ) ) {
		self = Vector3( 0, 0, 0 );
	}
}
typedef ReferenceCaller<Vector3, void(const char*), Vector3_importString> Vector3ImportStringCaller;
inline void Vector3_exportString( const Vector3& self, const StringImportCallback& importer ){
	char buffer[64];
	sprintf( buffer, "%g %g %g", self[0], self[1], self[2] );
	importer( buffer );
}
typedef ConstReferenceCaller<Vector3, void(const StringImportCallback&), Vector3_exportString> Vector3ExportStringCaller;



template<typename FirstArgument, typename Caller, typename FirstConversion>
class ImportConvert1
{
public:
	static void thunk( void* environment, FirstArgument firstArgument ){
		Caller::thunk( environment, FirstConversion( firstArgument ) );
	}
};


class CopiedStringFromString
{
	CopiedString m_value;
public:
	CopiedStringFromString( const char* string ){
		CopiedString_importString( m_value, string );
	}
	operator CopiedString() const
	{
		return m_value;
	}
};

inline void CopiedString_toString( const StringImportCallback& self, CopiedString value ){
	CopiedString_exportString( value, self );
}
typedef ConstReferenceCaller<StringImportCallback, void(CopiedString), CopiedString_toString> CopiedStringToString;


template<typename Caller>
inline StringImportCallback makeCopiedStringStringImportCallback( const Caller& caller ){
	return StringImportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringImportCallback, 0>, Caller, CopiedStringFromString>::thunk );
}

template<typename Caller>
inline StringExportCallback makeCopiedStringStringExportCallback( const Caller& caller ){
	return StringExportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringExportCallback, 0>, Caller, CopiedStringToString>::thunk );
}


class BoolFromString
{
	bool m_value;
public:
	BoolFromString( const char* string ){
		Bool_importString( m_value, string );
	}
	operator bool() const
	{
		return m_value;
	}
};

inline void Bool_toString( const StringImportCallback& self, bool value ){
	Bool_exportString( value, self );
}
typedef ConstReferenceCaller<StringImportCallback, void(bool), Bool_toString> BoolToString;


template<typename Caller>
inline StringImportCallback makeBoolStringImportCallback( const Caller& caller ){
	return StringImportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringImportCallback, 0>, Caller, BoolFromString>::thunk );
}

template<typename Caller>
inline StringExportCallback makeBoolStringExportCallback( const Caller& caller ){
	return StringExportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringExportCallback, 0>, Caller, BoolToString>::thunk );
}


class IntFromString
{
	int m_value;
public:
	IntFromString( const char* string ){
		Int_importString( m_value, string );
	}
	operator int() const
	{
		return m_value;
	}
};

inline void Int_toString( const StringImportCallback& self, int value ){
	Int_exportString( value, self );
}
typedef ConstReferenceCaller<StringImportCallback, void(int), Int_toString> IntToString;


template<typename Caller>
inline StringImportCallback makeIntStringImportCallback( const Caller& caller ){
	return StringImportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringImportCallback, 0>, Caller, IntFromString>::thunk );
}

template<typename Caller>
inline StringExportCallback makeIntStringExportCallback( const Caller& caller ){
	return StringExportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringExportCallback, 0>, Caller, IntToString>::thunk );
}



class SizeFromString
{
	std::size_t m_value;
public:
	SizeFromString( const char* string ){
		Size_importString( m_value, string );
	}
	operator std::size_t() const
	{
		return m_value;
	}
};

inline void Size_toString( const StringImportCallback& self, std::size_t value ){
	Size_exportString( value, self );
}
typedef ConstReferenceCaller<StringImportCallback, void(std::size_t), Size_toString> SizeToString;


template<typename Caller>
inline StringImportCallback makeSizeStringImportCallback( const Caller& caller ){
	return StringImportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringImportCallback, 0>, Caller, SizeFromString>::thunk );
}

template<typename Caller>
inline StringExportCallback makeSizeStringExportCallback( const Caller& caller ){
	return StringExportCallback( caller.getEnvironment(), ImportConvert1<get_argument<StringExportCallback, 0>, Caller, SizeToString>::thunk );
}
