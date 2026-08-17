// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VUmicro.h"
#include "VMManager.h"

#include "DebugTools/VUBreakpoints.h"

#include <cfenv>

extern void _vuFlushAll(VURegs* VU);

static void _vu0ExecUpper(VURegs* VU, u32* ptr)
{
	VU->code = ptr[1];
	IdebugUPPER(VU0);
	VU0_UPPER_OPCODE[VU->code & 0x3f]();
}

static void _vu0ExecLower(VURegs* VU, u32* ptr)
{
	VU->code = ptr[0];
	IdebugLOWER(VU0);
	VU0_LOWER_OPCODE[VU->code >> 25]();
}

int vu0branch = 0;
static void _vu0Exec(VURegs* VU)
{
	_VURegsNum lregs;
	_VURegsNum uregs;
	u32* ptr;

	ptr = (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
	VU->VI[REG_TPC].UL += 8;

	if (ptr[1] & 0x40000000) // E flag
	{
		VU->ebit = 2;
	}
	if (ptr[1] & 0x20000000 && VU == &VU0) // M flag
	{
		VU->flags |= VUFLAG_MFLAGSET;
		//		Console.WriteLn("fixme: M flag set");
	}
	if (ptr[1] & 0x10000000) // D flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x4)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x2;
			hwIntcIrq(INTC_VU0);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) // T flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x8)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x4;
			hwIntcIrq(INTC_VU0);
			VU->ebit = 1;
		}
	}

	VU->code = ptr[1];
	VU0regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);

	u64 cyclesBeforeOp = VU0.cycle - 1;

	_vuTestUpperStalls(VU, &uregs);

	/* check upper flags */
	if (ptr[1] & 0x80000000) // I flag
	{
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU0.cycle - cyclesBeforeOp), VU->VIBackupCycles);

		_vu0ExecUpper(VU, ptr);

		VU->VI[REG_I].UL = ptr[0];
		memset(&lregs, 0, sizeof(lregs));
	}
	else
	{
		VECTOR _VF;
		VECTOR _VFc;
		REG_VI _VI;
		REG_VI _VIc;
		int vfreg = 0;
		int vireg = 0;
		int discard = 0;

		VU->code = ptr[0];
		lregs.cycles = 0;
		VU0regs_LOWER_OPCODE[VU->code >> 25](&lregs);
		_vuTestLowerStalls(VU, &lregs);

		_vuTestPipes(VU);
		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU0.cycle - cyclesBeforeOp), VU->VIBackupCycles);
		vu0branch = lregs.pipe == VUPIPE_BRANCH;

		if (uregs.VFwrite)
		{
			if (lregs.VFwrite == uregs.VFwrite)
			{
				//				Console.Warning("*PCSX2*: Warning, VF write to the same reg in both lower/upper cycle");
				discard = 1;
			}
			if (lregs.VFread0 == uregs.VFwrite ||
				lregs.VFread1 == uregs.VFwrite)
			{
				//				Console.WriteLn("saving reg %d at pc=%x", i, VU->VI[REG_TPC].UL);
				_VF = VU->VF[uregs.VFwrite];
				vfreg = uregs.VFwrite;
			}
		}
		if (uregs.VIread & (1 << REG_CLIP_FLAG))
		{
			if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
			{
				//Console.Warning("*PCSX2*: Warning, VI write to the same reg in both lower/upper cycle");
				discard = 1;
			}
			if (lregs.VIread & (1 << REG_CLIP_FLAG))
			{
				_VI = VU0.VI[REG_CLIP_FLAG];
				vireg = REG_CLIP_FLAG;
			}
		}

		_vu0ExecUpper(VU, ptr);

		if (discard == 0)
		{
			if (vfreg)
			{
				_VFc = VU->VF[vfreg];
				VU->VF[vfreg] = _VF;
			}
			if (vireg)
			{
				_VIc = VU->VI[vireg];
				VU->VI[vireg] = _VI;
			}

			_vu0ExecLower(VU, ptr);

			if (vfreg)
			{
				VU->VF[vfreg] = _VFc;
			}
			if (vireg)
			{
				VU->VI[vireg] = _VIc;
			}
		}
	}

	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		_vuClearFMAC(VU);

	_vuAddUpperStalls(VU, &uregs);
	_vuAddLowerStalls(VU, &lregs);

	if (VU->branch > 0)
	{
		if (VU->branch-- == 1)
		{
			VU->VI[REG_TPC].UL = VU->branchpc;

			if (VU->takedelaybranch)
			{
				DevCon.Warning("VU0 - Branch/Jump in Delay Slot");
				VU->branch = 1;
				VU->branchpc = VU->delaybranchpc;
				VU->takedelaybranch = false;
			}
		}
	}

	if (VU->ebit > 0)
	{
		if (VU->ebit-- == 1)
		{
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			VU0.VI[REG_VPU_STAT].UL &= ~0x1; /* E flag */
			vif0Regs.stat.VEW = false;
		}
	}

	// Progress the write position of the FMAC pipeline by one place
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;
}

void vu0Exec(VURegs* VU)
{
	VU0.VI[REG_TPC].UL &= VU0_PROGMASK;
	VU->cycle++;
	_vu0Exec(VU);

	if (VU->VI[0].UL != 0)
		DbgCon.Error("VI[0] != 0!!!!\n");
	if (VU->VF[0].f.x != 0.0f)
		DbgCon.Error("VF[0].x != 0.0!!!!\n");
	if (VU->VF[0].f.y != 0.0f)
		DbgCon.Error("VF[0].y != 0.0!!!!\n");
	if (VU->VF[0].f.z != 0.0f)
		DbgCon.Error("VF[0].z != 0.0!!!!\n");
	if (VU->VF[0].f.w != 1.0f)
		DbgCon.Error("VF[0].w != 1.0!!!!\n");
}

// --------------------------------------------------------------------------------------
//  VU0microInterpreter
// --------------------------------------------------------------------------------------

InterpVU0 CpuIntVU0;

InterpVU0::InterpVU0()
{
	m_Idx = 0;
	IsInterpreter = true;
}

void InterpVU0::Reset()
{
	DevCon.Warning("VU0 Int Reset");
	VU0.fmacwritepos = 0;
	VU0.fmacreadpos = 0;
	VU0.fmaccount = 0;
	VU0.ialuwritepos = 0;
	VU0.ialureadpos = 0;
	VU0.ialucount = 0;
}
void InterpVU0::SetStartPC(u32 startPC)
{
	VU0.start_pc = startPC;
}

void InterpVU0::Step()
{
	vu0Exec(&VU0);
}

void InterpVU0::Execute(u32 cycles)
{
	const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);

	// PCSX2-MCP DEBUG: live heartbeat so a single abnormally-long Execute() call
	// (e.g. a VU0 microprogram that never sets its E-bit) shows up in the log
	// WHILE it's happening, not just after it finally returns. Remove once the
	// freeze investigation is done.
	u32 debug_heartbeat_counter = 0;
	constexpr u32 DEBUG_HEARTBEAT_INTERVAL = 2000000;

	VU0.VI[REG_TPC].UL <<= 3;
	VU0.flags &= ~VUFLAG_MFLAGSET;
	u64 startcycles = VU0.cycle;
	while ((VU0.cycle - startcycles) < cycles)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
		{
			// Branches advance the PC to the new location if there was a branch in the E-Bit delay slot
			if (VU0.branch)
			{
				VU0.VI[REG_TPC].UL = VU0.branchpc;
				VU0.branch = 0;
			}
			break;
		}
		if (VU0.flags & VUFLAG_MFLAGSET)
			break;

		// VU-side breakpoint check (VUBreakpoints.h) - only reachable while
		// VU0 is running interpreted, which is exactly the case we're in
		// here (InterpVU0::Execute). TPC is still left-shifted by 3 at this
		// point (see the `<<= 3` above), i.e. already a byte offset matching
		// what VUBreakpoints::Add() expects. Masked with VU0_PROGMASK to match
		// what vu0Exec() itself does to TPC one line into its own call - our
		// check runs before that, so it needs to replicate it or a wrapped
		// TPC could compare against an out-of-range address and never hit.
		if (VUBreakpoints::Check(0, VU0.VI[REG_TPC].UL & VU0_PROGMASK))
		{
			// Do NOT call VMManager::SetPaused()/Cpu->ExitExecution() from here.
			// This function is deeply nested inside the recompiled EE call stack
			// (recExecute -> ... -> VCALLMS -> _vu0FinishMicro -> _vu0run -> here)
			// and holds its own RAII guard (fpcr_backup, above) that must be
			// allowed to unwind normally first - fastjmp'ing past it here would
			// skip its destructor and leave the wrong FP control register (MXCSR)
			// active for all subsequent EE math. VUBreakpoints::Check() already
			// latched s_triggered; the actual pause + safe stack unwind happens
			// one level up, in _vu0run() (VU0.cpp), once this call has returned
			// and that guard's destructor has already run.
			//
			// PCSX2-MCP FIX: mirror the E-Bit-finished branch above - if a branch
			// was pending in the delay slot when this breakpoint hit, resolve it
			// now rather than leaving VU0.branch/branchpc stale. Otherwise the
			// *next* Execute() call (after resume) can start from an unresolved
			// branch state, which was a plausible source of the post-resume
			// infinite loops seen during the freeze investigation.
			if (VU0.branch)
			{
				VU0.VI[REG_TPC].UL = VU0.branchpc;
				VU0.branch = 0;
			}
			break;
		}

		vu0Exec(&VU0);

		if ((++debug_heartbeat_counter % DEBUG_HEARTBEAT_INTERVAL) == 0)
		{
			Console.WriteLnFmt("[PCSX2-MCP DEBUG] InterpVU0::Execute() heartbeat: {} instructions so far this call, TPC=0x{:04X}, VPU_STAT=0x{:X}, cyclesSoFar={}/{}",
				debug_heartbeat_counter, VU0.VI[REG_TPC].UL, VU0.VI[REG_VPU_STAT].UL, (VU0.cycle - startcycles), cycles);
		}
	}
	VU0.VI[REG_TPC].UL >>= 3;

	if (EmuConfig.Speedhacks.EECycleRate != 0 && (!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
	{
		u64 cycle_change = VU0.cycle - startcycles;
		VU0.cycle -= cycle_change;
		switch (std::min(static_cast<int>(EmuConfig.Speedhacks.EECycleRate), static_cast<int>(cycle_change)))
		{
			case -3: // 50%
				cycle_change *= 2.0f;
				break;
			case -2: // 60%
				cycle_change *= 1.6666667f;
				break;
			case -1: // 75%
				cycle_change *= 1.3333333f;
				break;
			case 1: // 130%
				cycle_change /= 1.3f;
				break;
			case 2: // 180%
				cycle_change /= 1.8f;
				break;
			case 3: // 300%
				cycle_change /= 3.0f;
				break;
			default:
				break;
		}
		VU0.cycle += cycle_change;
	}

	VU0.nextBlockCycles = (VU0.cycle - cpuRegs.cycle) + 1;
}
