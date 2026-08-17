// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: MIT
//
// Lightweight VU microcode breakpoint list.
//
// IMPORTANT CAVEAT: this is checked directly from the VU0/VU1 *interpreters*
// (VU0microInterp.cpp / VU1microInterp.cpp), on every instruction fetch.
// PCSX2's default execution path for VU0/VU1 is the microVU JIT recompiler,
// not the interpreter - and unlike the EE/IOP recompilers (which do insert
// breakpoint-check bailouts at compile time, see R5900.cpp's
// isBreakpointNeeded()), microVU has no such mechanism. These breakpoints
// will simply never fire while the affected VU is running under its
// recompiler.
//
// To use: System > Emulation > uncheck "Enable VU0 Recompiler" and/or
// "Enable VU1 Recompiler" for whichever VU you're setting a breakpoint on.
// This is a real (and real-sounding) performance cost - interpreted VU is
// much slower than JIT'd VU - so treat it like gdb's "software breakpoints
// only" mode: correct, but not free, and only for the VU you're actively
// investigating.
//
// A second, separate caveat: when Multi-Threaded VU1 (MTVU) is enabled,
// VU1's interpreter runs on its own dedicated "VU" thread, not the main EE
// CPU thread that DebugServer's runOnCpuBlocking() marshals onto. Add/
// Remove/List are still safe to call from there (this class is internally
// mutex-guarded), but be aware the *pause* that a hit triggers is crossing
// threads, same as it does for every other multi-threaded VMManager call in
// this codebase.

#pragma once

#include "common/Pcsx2Types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class VUBreakpoints
{
public:
	// vuIndex: 0 or 1.
	// addr: byte offset into that VU's micro program memory (VU0: 0..0xFFC,
	// VU1: 0..0x3FF8). VU instructions are always fetched 8 bytes at a time
	// starting from an 8-aligned TPC, so a non-8-aligned address will simply
	// never be hit - Add() does not reject it, but callers should align it.
	static void Add(int vuIndex, u32 addr, const std::string& description = std::string());
	static void Remove(int vuIndex, u32 addr);
	static void Clear(int vuIndex);
	static std::vector<std::pair<u32, std::string>> List(int vuIndex);

	// Called from the VU0/VU1 interpreters on every instruction fetch, before
	// the instruction at `addr` executes. Returns true if a breakpoint is set
	// there. The caller (the interpreter) is responsible for actually pausing
	// the VM and bailing out of its exec loop - this function only decides
	// whether one fired, mirroring how CBreakPoints::IsAddressBreakPoint /
	// SetBreakpointTriggered split the same two jobs for EE/IOP.
	static bool Check(int vuIndex, u32 addr);

	// Latches from the most recent Check() that returned true, so
	// DebugServer (which isn't on the CPU/VU thread) can report afterwards
	// which VU/address actually triggered the pause it observes.
	static bool WasTriggered();
	static void ClearTriggered();
	static int GetTriggeredVU();
	static u32 GetTriggeredAddress();

private:
	static std::mutex s_mutex;
	static std::vector<std::pair<u32, std::string>> s_breakpoints[2];

	static std::atomic<bool> s_triggered;
	static std::atomic<int> s_triggeredVU;
	static std::atomic<u32> s_triggeredAddr;
};
