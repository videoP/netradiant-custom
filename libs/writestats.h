/*
   Where a map save's time goes, split three ways.

   Saving the 1 GB test map takes ~40s and nothing at all is known about what
   that is made of. Two candidates, and they want completely different fixes:
   the writer emits ~420 million one-byte-or-so write() calls and ~102 million
   snprintf()s for the floats, but the same traversal also calls
   evaluateBRep() on every brush, which for a map whose windings are not built
   is the whole b-rep over again. Guessing between two candidates has a poor
   record in this codebase, hence measuring instead.

     brep    Brush::evaluateBRep(), inside the exporter, per brush
     export  all of BrushTokenExporter::exportTokens - formatting and handing
             bytes to the stream, and so io as well
     io      fwrite() itself, inside TextFileOutputStream

   What is left of the save timer after export is the scene traversal and the
   entity and patch writing, which happen in the map module.
 */

#pragma once

#include "timer.h"

#include <cstddef>

struct WriteStats
{
	double brepSeconds;
	double exportSeconds;
	double ioSeconds;
	std::size_t brushes;
	std::size_t writeCalls;   // TextOutputStream::write() - the token writer's
	std::size_t fwrites;      // what reaches stdio after buffering
	std::size_t bytes;
};

/* Inline rather than extern: textfilestream.h is included by the module DLLs
   too, and they must not need a symbol out of radiant.exe to link. Everything
   on the save path is inside radiant.exe, so it is one copy where it counts. */
inline WriteStats g_writeStats;

/// \brief Adds the lifetime of the scope to one of the totals above.
class WriteTimerScope
{
	double& m_total;
	Timer m_timer;
public:
	WriteTimerScope( double& total ) : m_total( total ){
	}
	~WriteTimerScope(){
		m_total += m_timer.elapsed_sec();
	}
};
