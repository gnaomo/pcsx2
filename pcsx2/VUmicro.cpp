// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"

BaseVUmicroCPU* CpuVU0 = nullptr;
BaseVUmicroCPU* CpuVU1 = nullptr;

__inline u32 CalculateMinRunCycles(u32 cycles, bool requiresAccurateCycles)
{
	// If we're running an interlocked COP2 operation
	// run for an exact amount of cycles
	if(requiresAccurateCycles)
		return cycles;

	// Allow a minimum of 16 cycles to avoid running small blocks
	// Running a block of like 3 cycles is highly inefficient
	// so while sync isn't tight, it's okay to run ahead a little bit.
	return std::max(16U, cycles);
}

// Executes a Block based on EE delta time
void BaseVUmicroCPU::ExecuteBlock(bool startUp)
{
	const u32& stat = VU0.VI[REG_VPU_STAT].UL;
	const int test = m_Idx ? 0x100 : 1;

	if (m_Idx && THREAD_VU1)
	{
		vu1Thread.Get_MTVUChanges();
		return;
	}

	if (!(stat & test))
	{
		// VU currently flushes XGKICK on VU1 end so no need for this, yet
		/*if (m_Idx == 1 && VU1.xgkickenable)
		{
			_vuXGKICKTransfer((cpuRegs.cycle - VU1.xgkicklastcycle), false);
		}*/
		return;
	}

	if (startUp)
	{
		Execute(CalculateMinRunCycles(0, false));
	}
	else // Continue Executing
	{
		u64 cycle = m_Idx ? VU1.cycle : VU0.cycle;
		s32 delta = (s32)(u32)(cpuRegs.cycle - cycle);

		if (delta > 0)
		{
			// PCSX2-MCP FIX: a VU-breakpoint-interrupted burst can leave this
			// VU's own cycle counter stalled for an extended real-time window
			// while cpuRegs.cycle keeps advancing normally from ordinary EE
			// execution in between, building up a large but entirely genuine
			// gap. Catching all of it up in a single Execute() call can take
			// an enormous, effectively unbounded amount of real time in the
			// (slow, uncompiled) interpreter - this was the actual mechanism
			// behind the apparent VU0-breakpoint "freeze": not a true infinite
			// loop, just one blocking call trying to pay off hundreds of
			// millions of cycles of accumulated debt at once. Cap how much any
			// single call catches up so the remainder is picked up gradually
			// across the many subsequent ExecuteBlock() calls that happen
			// naturally during normal execution, instead of blocking the whole
			// EE thread for one huge call. Total cycles executed and final
			// state are unaffected - this only changes how the catch-up is
			// chunked over time.
			constexpr s32 MAX_CATCHUP_CYCLES_PER_CALL = 4'000'000;
			Execute(CalculateMinRunCycles(std::min(delta, MAX_CATCHUP_CYCLES_PER_CALL), false));
		}
	}
}

// This function is called by VU0 Macro (COP2) after transferring some
// EE data to VU0's registers. We want to run VU0 Micro right after this
// to ensure that the register is used at the correct time.
// This fixes spinning/hanging in some games like Ratchet and Clank's Intro.
void BaseVUmicroCPU::ExecuteBlockJIT(BaseVUmicroCPU* cpu, bool interlocked)
{
	const u32& stat = VU0.VI[REG_VPU_STAT].UL;
	constexpr int test = 1;

	if (stat & test)
	{ // VU is running
		s64 delta = (s64)(u64)(cpuRegs.cycle - VU0.cycle);

		if (delta > 0)
		{
			// PCSX2-MCP FIX: same uncapped catch-up issue as ExecuteBlock()
			// above - this second, separate call path (invoked from JIT-emitted
			// code right after a VU0 COP2 macro transfer) had the identical
			// pattern and turned out to be the one actually hit during the
			// VU0-breakpoint freeze repro (the ExecuteBlock() fix alone wasn't
			// enough - this path was still uncapped). Skip the cap when
			// interlocked is true though: that flag means the caller needs an
			// exact, accurate cycle count for a specific interlocked COP2 op
			// (see CalculateMinRunCycles's requiresAccurateCycles), so chunking
			// it would be incorrect there, not just slow.
			constexpr s64 MAX_CATCHUP_CYCLES_PER_CALL = 4'000'000;
			const s64 cappedDelta = interlocked ? delta : std::min(delta, MAX_CATCHUP_CYCLES_PER_CALL);
			cpu->Execute(CalculateMinRunCycles(cappedDelta, interlocked)); // Execute the time since the last call
		}
	}
}
