// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: MIT

#include "VUBreakpoints.h"

#include <algorithm>

std::mutex VUBreakpoints::s_mutex;
std::vector<std::pair<u32, std::string>> VUBreakpoints::s_breakpoints[2];

std::atomic<bool> VUBreakpoints::s_triggered{false};
std::atomic<int> VUBreakpoints::s_triggeredVU{-1};
std::atomic<u32> VUBreakpoints::s_triggeredAddr{0};

void VUBreakpoints::Add(int vuIndex, u32 addr, const std::string& description)
{
	if (vuIndex != 0 && vuIndex != 1)
		return;

	std::lock_guard<std::mutex> lock(s_mutex);
	auto& list = s_breakpoints[vuIndex];
	auto it = std::find_if(list.begin(), list.end(), [addr](const auto& bp) { return bp.first == addr; });
	if (it != list.end())
		it->second = description;
	else
		list.emplace_back(addr, description);
}

void VUBreakpoints::Remove(int vuIndex, u32 addr)
{
	if (vuIndex != 0 && vuIndex != 1)
		return;

	std::lock_guard<std::mutex> lock(s_mutex);
	auto& list = s_breakpoints[vuIndex];
	list.erase(std::remove_if(list.begin(), list.end(), [addr](const auto& bp) { return bp.first == addr; }), list.end());
}

void VUBreakpoints::Clear(int vuIndex)
{
	if (vuIndex != 0 && vuIndex != 1)
		return;

	std::lock_guard<std::mutex> lock(s_mutex);
	s_breakpoints[vuIndex].clear();
}

std::vector<std::pair<u32, std::string>> VUBreakpoints::List(int vuIndex)
{
	if (vuIndex != 0 && vuIndex != 1)
		return {};

	std::lock_guard<std::mutex> lock(s_mutex);
	return s_breakpoints[vuIndex]; // copy out - caller may be off the CPU thread
}

bool VUBreakpoints::Check(int vuIndex, u32 addr)
{
	// Always lock rather than fast-pathing on an unlocked emptiness check:
	// VU1 under MTVU runs this from a dedicated VU thread, separate from the
	// EE CPU thread that Add()/Remove() are marshaled onto via
	// runOnCpuBlocking() in DebugServer.cpp, so an unlocked read of the
	// vector here would be a genuine data race, not just a benign staleness
	// window. This runs once per VU instruction *only* while that VU is
	// interpreted (see the header comment) - already the slow path, so the
	// extra lock isn't the bottleneck.
	std::lock_guard<std::mutex> lock(s_mutex);
	auto& list = s_breakpoints[vuIndex];
	if (list.empty())
		return false;

	auto it = std::find_if(list.begin(), list.end(), [addr](const auto& bp) { return bp.first == addr; });
	if (it == list.end())
		return false;

	s_triggeredVU = vuIndex;
	s_triggeredAddr = addr;
	s_triggered = true;
	return true;
}

bool VUBreakpoints::WasTriggered() { return s_triggered.load(); }
void VUBreakpoints::ClearTriggered() { s_triggered = false; }
int VUBreakpoints::GetTriggeredVU() { return s_triggeredVU.load(); }
u32 VUBreakpoints::GetTriggeredAddress() { return s_triggeredAddr.load(); }
