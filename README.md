> ## ⚠️ Fork Notice: LLM-Generated Changes
>
> **This fork adds an experimental debug bridge for external tooling (e.g. AI-assisted debugging) on top of upstream PCSX2.**
>
> **Added / changed in this fork:**
> - `pcsx2/DebugTools/DebugServer.{cpp,h}` — a new local TCP/JSON server exposing register access, memory read/write, breakpoints, disassembly, VU0/VU1 state, save-state control, and related debug operations to external tools.
> - `pcsx2/DebugTools/VUBreakpoints.{cpp,h}` — VU0/VU1 microcode address breakpoints (interpreter-only; not supported under the microVU recompiler — see code comments).
> - `pcsx2/VMManager.cpp` — starts/stops the debug server alongside the CPU thread, plus temporary diagnostic logging around VU0/VU1 recompiler↔interpreter backend swaps for an in-progress investigation into a VU-breakpoint freeze issue.
> - Small supporting edits to `CMakeLists.txt`, `Config.h`, `GSCapture.cpp`, `PINE.cpp`, `Pcsx2Config.cpp`, `VU0.cpp`, `VU0microInterp.cpp`, `VU1micro.cpp`, `VU1microInterp.cpp`, `VUmicro.cpp` to wire the above in.
>
> **⚠️ LLM disclosure:** All of the changes described above were written entirely by an LLM (AI coding assistant) and have **not been reviewed, audited, or verified by the repository owner**. Treat this code as untrusted / unreviewed. It is kept here purely as a personal backup of work-in-progress and is **not intended for upstream contribution or production use**.
>
> Everything below this notice is the unmodified upstream PCSX2 README.

# PCSX2

![Windows Build Status](https://img.shields.io/github/actions/workflow/status/PCSX2/pcsx2/windows_build_matrix.yml?label=%F0%9F%96%A5%EF%B8%8F%20Windows%20Builds)
![Linux Build Status](https://img.shields.io/github/actions/workflow/status/PCSX2/pcsx2/linux_build_matrix.yml?label=%F0%9F%90%A7%20Linux%20Builds)
![MacOS Build Status](https://img.shields.io/github/actions/workflow/status/PCSX2/pcsx2/macos_build_matrix.yml?label=%F0%9F%8D%8E%20MacOS%20Builds)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/1f7c0d75fec74d6daa6adb084e5b4f71)](https://app.codacy.com/gh/PCSX2/pcsx2/dashboard?utm_source=github.com&utm_medium=referral&utm_content=PCSX2/pcsx2&utm_campaign=Badge_Grade)
[![Discord Server](https://img.shields.io/discord/309643527816609793?color=%235CA8FA&label=PCSX2%20Discord&logo=discord&logoColor=white)](https://discord.com/invite/TCz3t9k)

PCSX2 is a free and open-source PlayStation 2 (PS2) emulator. Its purpose is to emulate the PS2's hardware, using a combination of MIPS CPU [Interpreters](<https://en.wikipedia.org/wiki/Interpreter_(computing)>), [Recompilers](https://en.wikipedia.org/wiki/Dynamic_recompilation) and a [Virtual Machine](https://en.wikipedia.org/wiki/Virtual_machine) which manages hardware states and PS2 system memory. This allows you to play PS2 games on your PC, with many additional features and benefits.

## Project Details

PCSX2 has been in development for more than 20 years. Past versions could only run a few public domain game demos, but newer versions can run most games at full speed, including popular titles such as Final Fantasy X and Devil May Cry 3. Visit the [PCSX2 compatibility list](https://pcsx2.net/compat/) to check the latest compatibility status of games (with more than 2500 titles tested).

Installers and binaries for both stable and nightly builds are available from [our website](https://pcsx2.net/downloads/).

## System Requirements

PCSX2 supports Windows, Linux, and Mac platforms. Our [setup documentation page](https://pcsx2.net/docs/setup/requirements) contains additional details on software and hardware requirements.

Please note that a BIOS dump from a legitimately-owned PS2 console is required to use the emulator. For more information, visit [this page](https://pcsx2.net/docs/setup/bios/).

## Contributing / Building

PCSX2 supports translation into other languages using [Crowdin](https://crowdin.com/project/pcsx2-emulator).

See the [Contribution Guide](https://pcsx2.net/docs/contributing/) for more info on how to contribute.
