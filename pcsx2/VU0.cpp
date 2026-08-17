// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/* TODO
 -Fix the flags Proper as they aren't handle now..
 -Add BC Table opcodes
 -Add Interlock in QMFC2,QMTC2,CFC2,CTC2
 -Finish instruction set
 -Bug Fixes!!!
*/

#include "Common.h"

#include <cmath>

#include "R5900OpcodeTables.h"
#include "VUmicro.h"
#include "Vif_Dma.h"
#include "MTVU.h"
#include "VMManager.h"
#include "DebugTools/VUBreakpoints.h"
#include <atomic>

#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

#define _Fsf_ ((cpuRegs.code >> 21) & 0x03)
#define _Ftf_ ((cpuRegs.code >> 23) & 0x03)

using namespace R5900;

void COP2_BC2() { Int_COP2BC2PrintTable[_Rt_]();}
void COP2_SPECIAL() { _vu0FinishMicro(); Int_COP2SPECIAL1PrintTable[_Funct_]();}

void COP2_SPECIAL2() {
	Int_COP2SPECIAL2PrintTable[(cpuRegs.code & 0x3) | ((cpuRegs.code >> 4) & 0x7c)]();
}

void COP2_Unknown()
{
	CPU_LOG("Unknown COP2 opcode called");
}

//****************************************************************************

__fi void _vu0run(bool breakOnMbit, bool addCycles, bool sync_only) {

	if (!(VU0.VI[REG_VPU_STAT].UL & 1)) return;

	// PCSX2-MCP DEBUG: per-call instrumentation for the VU0-breakpoint freeze
	// investigation. Cheap (once per _vu0run call, not per VU instruction).
	// Remove once resolved.
	static std::atomic<u64> s_debug_call_count{0};
	const u64 debug_call_id = s_debug_call_count.fetch_add(1, std::memory_order_relaxed);
	const bool debug_is_interpreter = (CpuVU0 == static_cast<BaseVUmicroCPU*>(&CpuIntVU0));
	const u32 debug_start_tpc = VU0.VI[REG_TPC].UL;
	u32 debug_iterations = 0;

	//VU0 is ahead of the EE and M-Bit is already encountered, so no need to wait for it, just catch up the EE
	if ((VU0.flags & VUFLAG_MFLAGSET) && breakOnMbit && (s64)(cpuRegs.cycle - VU0.cycle) <= 0)
	{
		cpuRegs.cycle = VU0.cycle;
		return;
	}

	if(!EmuConfig.Cpu.Recompiler.EnableEE)
		intUpdateCPUCycles();

	u64 startcycle = cpuRegs.cycle;
	s32 runCycles  = 0x7fffffff;

	if (sync_only)
	{
		runCycles  = (s64)(cpuRegs.cycle - VU0.cycle);

		if (runCycles < 0)
			return;
	}

	// PCSX2-MCP DEBUG: dump the exact call-time state right before we hand
	// off to the interpreter, so we can see what runCycles/sync_only/the
	// VU0.cycle-vs-cpuRegs.cycle gap actually are for a call that goes on
	// to become one of the long-running ones - the heartbeat log inside
	// Execute() shows a "cycles" bound that doesn't match the fixed
	// 0x7fffffff we'd expect for the non-sync_only path, so something here
	// needs directly observing rather than inferring from source. Remove
	// once resolved.
	if (sync_only || runCycles != 0x7fffffff || (s64)(cpuRegs.cycle - VU0.cycle) > 1000000)
	{
		Console.WriteLnFmt("[PCSX2-MCP DEBUG] _vu0run #{}: ANOMALOUS pre-Execute runCycles={}, sync_only={}, breakOnMbit={}, addCycles={}, VU0.cycle={}, cpuRegs.cycle={}, gap={}, VIBackupCycles={}",
			debug_call_id, runCycles, sync_only, breakOnMbit, addCycles, VU0.cycle, cpuRegs.cycle,
			(s64)(cpuRegs.cycle - VU0.cycle), VU0.VIBackupCycles);
	}

	do { // Run VU until it finishes or M-Bit
		CpuVU0->Execute(runCycles);
		debug_iterations++;

		// A VU0 breakpoint (VUBreakpoints.h) can only be detected from inside
		// CpuVU0->Execute() (the interpreter's own per-instruction loop), which
		// just returned to us here. It deliberately did NOT act on it beyond
		// breaking out of its own loop - see the comment in
		// VU0microInterp.cpp's InterpVU0::Execute() for why. We're now back in
		// the caller's frame, past that function's RAII guards, so this is the
		// first safe place to actually pause: stop the do-while from re-running
		// Execute() at the exact same (unadvanced) TPC - which would otherwise
		// spin as fast as the CPU can go, repeatedly re-triggering the same
		// breakpoint - and unwind the EE recompiler's call stack the same way
		// an EE-side breakpoint hit does (see recError() etc. in iR5900.cpp),
		// so PCSX2's own pause machinery (MTGS sync, Host::OnVMPaused(), and
		// critically the CPU thread's own event loop that services DebugServer
		// commands including resume) actually takes over instead of silently
		// falling back into more EE code as if nothing happened.
		if (VUBreakpoints::WasTriggered())
			break;
	} while ((VU0.VI[REG_VPU_STAT].UL & 1)						// E-bit Termination
	  &&	!sync_only && (!breakOnMbit || (!(VU0.flags & VUFLAG_MFLAGSET) && (s32)(cpuRegs.cycle - VU0.cycle) > 0)));	// M-bit Break

	if (VUBreakpoints::WasTriggered())
	{
		VUBreakpoints::ClearTriggered();
		VMManager::SetPaused(true);
		Console.WriteLnFmt("[PCSX2-MCP DEBUG] _vu0run #{}: VU breakpoint pause fired (backend={}, startTPC=0x{:04X}, iterations={})",
			debug_call_id, debug_is_interpreter ? "interp" : "recomp", debug_start_tpc, debug_iterations);
		// Deliberately NOT calling Cpu->ExitExecution() here. That forces an
		// immediate fastjmp-based unwind of the EE recompiler's dispatch loop,
		// which is only meant to be triggered from the recompiler's own
		// designated call sites (recError() etc. in iR5900.cpp) that pair it
		// with their own internal exit-state bookkeeping. Called from here -
		// a VU-interpreter-driven call chain the recompiler wasn't written to
		// expect - it left that bookkeeping in a state where every subsequent
		// block re-exits immediately forever (confirmed by testing: game
		// freezes permanently after the first VU breakpoint pause/resume
		// cycle, survives toggling the VU0 recompiler setting and manual
		// pause/unpause). We don't need it anyway: the do-while break above
		// already fixes the actual livelock (no more re-entering Execute() at
		// the same unadvanced TPC), and a single SetPaused(true) call is
		// picked up shortly after by the EE recompiler's own existing periodic
		// checkpoint - the same one the normal UI Pause button already relies
		// on - without needing to force anything.
	}

	// PCSX2-MCP DEBUG: log the tail end of this call whenever it took an
	// unusual number of do-while iterations (>1000) or more than ~1/10th of a
	// PS2 EE-second of VU0 cycles in one go - either would be a strong signal
	// this call, not the surrounding infra, is where time is disappearing.
	// PCSX2-MCP FIX: same u64-underflow issue as the cpuRegs.cycle catch-up
	// above - use the signed delta so a no-progress call (VU0.cycle behind
	// startcycle) reads as a small/negative number instead of a bogus huge
	// one that made this fire as a false "SLOW CALL" on nearly every hit.
	const s64 debug_cycles_consumed = static_cast<s64>(VU0.cycle - startcycle);
	if (debug_iterations > 1000 || debug_cycles_consumed > 29491200ll)
	{
		Console.WriteLnFmt("[PCSX2-MCP DEBUG] _vu0run #{}: SLOW CALL (backend={}, startTPC=0x{:04X}, endTPC=0x{:04X}, iterations={}, cyclesConsumed={}, stillRunning={})",
			debug_call_id, debug_is_interpreter ? "interp" : "recomp", debug_start_tpc, VU0.VI[REG_TPC].UL, debug_iterations, debug_cycles_consumed,
			(VU0.VI[REG_VPU_STAT].UL & 1) != 0);
	}

	// Add cycles if called from EE's COP2
	if (addCycles)
	{
		// PCSX2-MCP FIX: (VU0.cycle - startcycle) is a u64 subtraction - if a VU
		// breakpoint caused this call (or a recent prior one) to bail out before
		// VU0.cycle made real progress, VU0.cycle can end up behind startcycle,
		// underflowing to a huge value and corrupting cpuRegs.cycle (which the
		// whole event-scheduling/pause-checkpoint system is driven off of).
		// Cast through s64 to recover the true signed delta and clamp negative
		// (no-progress) cases to 0 instead of adding a huge wrapped number.
		const s64 vu0_cycle_delta = static_cast<s64>(VU0.cycle - startcycle);
		cpuRegs.cycle += (vu0_cycle_delta > 0) ? static_cast<u64>(vu0_cycle_delta) : 0;
		CpuVU1->ExecuteBlock(0); // Catch up VU1 as it's likely fallen behind

		if(VU0.VI[REG_VPU_STAT].UL & 1)
			cpuSetNextEventDelta(4);
	}
}

void _vu0WaitMicro()   { _vu0run(1, 1, 0); } // Runs VU0 Micro Until E-bit or M-Bit End
void _vu0FinishMicro() { _vu0run(0, 1, 0); } // Runs VU0 Micro Until E-Bit End
void vu0Finish()	   { _vu0run(0, 0, 0); } // Runs VU0 Micro Until E-Bit End (doesn't stall EE)
void vu0Sync()		   { _vu0run(0, 0, 1); } // Runs VU0 until it catches up

namespace R5900 {
namespace Interpreter{
namespace OpcodeImpl
{
	void LQC2() {
		vu0Sync();
		u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + (s16)cpuRegs.code;
		if (_Ft_) {
			memRead128(addr, VU0.VF[_Ft_].UQ);
		} else {
			u128 val;
 			memRead128(addr, val);
		}
	}

	// Asadr.Changed
	//TODO: check this
	// HUH why ? doesn't make any sense ...
	void SQC2() {
		vu0Sync();
		u32 addr = _Imm_ + cpuRegs.GPR.r[_Rs_].UL[0];
		memWrite128(addr, VU0.VF[_Ft_].UQ);
	}
}}}


void QMFC2() {
	vu0Sync();

	if (cpuRegs.code & 1) {
		_vu0FinishMicro();
	}

	if (_Rt_ == 0) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = VU0.VF[_Fs_].UD[0];
	cpuRegs.GPR.r[_Rt_].UD[1] = VU0.VF[_Fs_].UD[1];
}

void QMTC2() {
	vu0Sync();

	if (cpuRegs.code & 1) {
		_vu0WaitMicro();
	}

	if (_Fs_ == 0) return;
	VU0.VF[_Fs_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0];
	VU0.VF[_Fs_].UD[1] = cpuRegs.GPR.r[_Rt_].UD[1];
}

void CFC2() {
	vu0Sync();

	if (cpuRegs.code & 1) {
		_vu0FinishMicro();
	}

	if (_Rt_ == 0) return;

	if (_Fs_ == REG_R)
		cpuRegs.GPR.r[_Rt_].UL[0] = VU0.VI[REG_R].UL & 0x7FFFFF;
	else
	{
		cpuRegs.GPR.r[_Rt_].UL[0] = VU0.VI[_Fs_].UL;

		if (VU0.VI[_Fs_].UL & 0x80000000)
			cpuRegs.GPR.r[_Rt_].UL[1] = 0xffffffff;
		else
			cpuRegs.GPR.r[_Rt_].UL[1] = 0;
	}

}

void CTC2() {
	vu0Sync();

	if (cpuRegs.code & 1) {
		_vu0WaitMicro();
	}

	if (_Fs_ == 0) return;

	switch(_Fs_) {
		case REG_MAC_FLAG: // read-only
		case REG_TPC:      // read-only
		case REG_VPU_STAT: // read-only
			break;
		case REG_R:
			VU0.VI[REG_R].UL = ((cpuRegs.GPR.r[_Rt_].UL[0] & 0x7FFFFF) | 0x3F800000);
			break;
		case REG_FBRST:
			VU0.VI[REG_FBRST].UL = cpuRegs.GPR.r[_Rt_].UL[0] & 0x0C0C;
			if (cpuRegs.GPR.r[_Rt_].UL[0] & 0x1) { // VU0 Force Break
				Console.Error("fixme: VU0 Force Break");
			}
			if (cpuRegs.GPR.r[_Rt_].UL[0] & 0x2) { // VU0 Reset
				//Console.WriteLn("fixme: VU0 Reset");
				vu0ResetRegs();
			}
			if (cpuRegs.GPR.r[_Rt_].UL[0] & 0x100) { // VU1 Force Break
				Console.Error("fixme: VU1 Force Break");
			}
			if (cpuRegs.GPR.r[_Rt_].UL[0] & 0x200) { // VU1 Reset
//				Console.WriteLn("fixme: VU1 Reset");
				vu1ResetRegs();
			}
			break;
		case REG_CMSAR1: // REG_CMSAR1
			vu1Finish(true);
			vu1ExecMicro(cpuRegs.GPR.r[_Rt_].US[0]);	// Execute VU1 Micro SubRoutine
			break;
		case REG_CLIP_FLAG:
			VU0.clipflag = cpuRegs.GPR.r[_Rt_].UL[0];
		default:
			VU0.VI[_Fs_].UL = cpuRegs.GPR.r[_Rt_].UL[0];
			break;
	}
}
