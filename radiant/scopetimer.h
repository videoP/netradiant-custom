/*
   Reports how long a scope took, to the console.

   Opening a map is several distinct phases and the total says nothing about
   which of them costs what. Guessing at that has a poor record here.
 */

#pragma once

#include "timer.h"
#include "itextstream.h"
#include "stream/textstream.h"

class ScopeTimer
{
	Timer m_timer;
	const char* m_message;
public:
	ScopeTimer( const char* message )
		: m_message( message ){
	}
	~ScopeTimer(){
		globalOutputStream() << m_message << " timer: " << FloatFormat( m_timer.elapsed_sec(), 5, 2 ) << " second(s) elapsed\n";
	}
};
