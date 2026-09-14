/*
   Reports how much memory the process is holding, to the console.

   The per-brush memory figures this replaces were worked out from struct
   layouts and an assumed allocator overhead, never measured. Load
   the same map twice - Large Maps options off, then on - and diff these two
   numbers instead.
 */

#pragma once

#include "itextstream.h"
#include "stream/textstream.h"

#include <cstddef>

/// \brief Resident and committed bytes, or 0 where the platform does not say.
struct MemoryUse
{
	std::size_t m_workingSet = 0;   // resident
	std::size_t m_privateBytes = 0; // committed, resident or not
};

MemoryUse MemoryUse_get();

/// \brief One line: "<what> memory: working set 1234 MB, private 1300 MB".
void MemoryUse_report( const char* what );


/// \brief Processor time burned and page faults taken, both counted since the
/// process started. Only differences between two readings mean anything.
///
/// Wall clock alone cannot tell a phase that is doing too much work from one
/// that is waiting on the pagefile, and once a map stops fitting in physical
/// memory that is the first question worth asking - a phase reporting a third
/// of its wall time as CPU was swapping, whatever its code looks like. Guessing
/// at this instead has a poor record here.
struct ProcessTime
{
	double m_cpuSeconds = 0;      // user + kernel
	std::size_t m_pageFaults = 0; // soft and hard alike; the platform does not separate them
	bool m_valid = false;         // false where the platform did not answer
};

ProcessTime ProcessTime_get();


/*! \brief One line splitting committed memory by what kind of thing holds it.

    The brush walk in brushmemory.cpp accounts for what it can reach from the
    scene, and on a large map several GB are left over. Before walking the heap
    block by block to find them - which over ~79M live blocks takes minutes -
    this says whether they are on the heap at all. It reads regions rather than
    blocks, so it costs milliseconds even across 55 GB.
 */
void VirtualMemory_report( const char* what );
