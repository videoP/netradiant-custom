/*
   See memreport.h.
 */

#include "memreport.h"

#include <cstdint>

#if defined( WIN32 )
#include <windows.h>
#include <psapi.h>
#endif

namespace
{
const std::size_t c_mb = 1024 * 1024;
}

MemoryUse MemoryUse_get(){
	MemoryUse use;
#if defined( WIN32 )
	PROCESS_MEMORY_COUNTERS_EX counters;
	counters.cb = sizeof( counters );
	if ( GetProcessMemoryInfo( GetCurrentProcess(),
	                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>( &counters ),
	                           sizeof( counters ) ) ) {
		use.m_workingSet = counters.WorkingSetSize;
		use.m_privateBytes = counters.PrivateUsage;
	}
#endif
	return use;
}

void MemoryUse_report( const char* what ){
	const MemoryUse use = MemoryUse_get();
	if ( use.m_workingSet == 0 ) {
		return; // platform did not answer; a zero would read as a measurement
	}
	/* private first: the working set is whatever Windows has left resident and
	   moves by gigabytes between identical runs, so it is the committed figure
	   that should be compared and quoted. */
	globalOutputStream() << what << " memory: private " << Unsigned( use.m_privateBytes / c_mb )
	                     << " MB committed, " << Unsigned( use.m_workingSet / c_mb )
	                     << " MB resident\n";
}

ProcessTime ProcessTime_get(){
	ProcessTime time;
#if defined( WIN32 )
	FILETIME created, exited, kernel, user;
	if ( GetProcessTimes( GetCurrentProcess(), &created, &exited, &kernel, &user ) ) {
		/* Both are counts of 100 ns ticks. FILETIME splits them across two
		   32-bit fields only because it predates 64-bit ones, so recombine
		   rather than reading either half as anything calendar-shaped. */
		const auto ticks = []( const FILETIME& t ){
			return ( static_cast<std::uint64_t>( t.dwHighDateTime ) << 32 ) | t.dwLowDateTime;
		};
		time.m_cpuSeconds = static_cast<double>( ticks( kernel ) + ticks( user ) ) * 1e-7;
		time.m_valid = true;
	}

	PROCESS_MEMORY_COUNTERS counters;
	counters.cb = sizeof( counters );
	if ( GetProcessMemoryInfo( GetCurrentProcess(), &counters, sizeof( counters ) ) ) {
		time.m_pageFaults = counters.PageFaultCount;
	}
#endif
	return time;
}

void VirtualMemory_report( const char* what ){
#if defined( WIN32 )
	std::size_t priv = 0, mapped = 0, image = 0, reserved = 0;

	const char* address = 0;
	MEMORY_BASIC_INFORMATION region;
	while ( VirtualQuery( address, &region, sizeof( region ) ) == sizeof( region ) )
	{
		if ( region.State == MEM_COMMIT ) {
			switch ( region.Type )
			{
			case MEM_PRIVATE: priv += region.RegionSize; break;
			case MEM_MAPPED: mapped += region.RegionSize; break;
			case MEM_IMAGE: image += region.RegionSize; break;
			}
		}
		else if ( region.State == MEM_RESERVE ) {
			reserved += region.RegionSize;
		}

		const char* const next = static_cast<const char*>( region.BaseAddress ) + region.RegionSize;
		if ( next <= address ) {
			break; // a zero-sized region, or the top of the address space wrapping
		}
		address = next;
	}

	/* "private" is everything the process allocated for itself: the heap, but
	   also thread stacks and any VirtualAlloc, the graphics driver's included -
	   so a gap that lands here is on the heap or in the driver, and a gap that
	   does not is neither. */
	globalOutputStream() << what << " address space: private " << Unsigned( priv / c_mb )
	                     << " MB, mapped files " << Unsigned( mapped / c_mb )
	                     << " MB, images " << Unsigned( image / c_mb )
	                     << " MB committed; " << Unsigned( reserved / c_mb ) << " MB reserved\n";
#endif
}
