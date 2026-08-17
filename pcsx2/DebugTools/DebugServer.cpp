// SPDX-FileCopyrightText: 2026 PS2Recomp Debug Bridge
// SPDX-License-Identifier: MIT
//
// PCSX2 Debug Server — JSON/TCP API wrapping full DebugInterface
// Protocol: newline-delimited JSON over TCP (port 21512)
//
// Request:  {"cmd":"read_registers","cpu":"ee","category":0}\n
// Response: {"ok":true,"data":{...}}\n
//
// Integration:
//   1. Drop DebugServer.h + DebugServer.cpp into pcsx2/DebugTools/
//   2. Add to CMakeLists.txt
//   3. Call DebugServer::Start() from VMManager::Internal::CPUThreadInitialize()
//   4. Call DebugServer::Stop() from VMManager::Internal::CPUThreadShutdown()
//   5. Call DebugServer::OnBreakpointHit() from breakpoint handler (optional, currently a no-op hook)
//
// NOTE (Linux build fixes vs. the original hallucinated draft of this file):
//   - DebugInterface exposes Read8/Write8/Read32/... (capitalized, from MemoryInterface),
//     NOT read8/write8/read32. Also `valid` is a bool*, not a bool&.
//   - MipsStackWalk::Walk() takes (cpu, pc, ra, sp, threadEntry) — 5 args, no stack-top arg.
//   - BiosThread has no StackTop() accessor, only EntryPoint().
//   - <unordered_map> and <chrono> are used but weren't included.

// ============================================================
// NOTE: This file uses a minimal inline JSON builder to avoid
// external dependencies. PCSX2 doesn't bundle nlohmann/json
// in all configurations.
// ============================================================

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#endif

#include "DebugServer.h"
#include "DebugInterface.h"
#include "Breakpoints.h"
#include "VUBreakpoints.h"
#include "Config.h"
#include "MipsStackWalk.h"
#include "MIPSAnalyst.h"
#include "MipsAssembler.h"
#include "SymbolGuardian.h"
#include "Host.h"
#include "VMManager.h"
#include "CDVD/CDVDcommon.h"
#include "common/Error.h"
#include "GameDatabase.h"
#include "Patch.h"
#include "VU.h"
#include "VUmicro.h"
#include "Debug.h"
#include "MTGS.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/GSState.h"
#include "GS/GSDrawingContext.h"
#include "GS/GSDrawingEnvironment.h"
#include "common/Image.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <set>
#include <sstream>
#include <algorithm>
#include <charconv>
#include <unordered_map>
#include <chrono>
#include <cctype>

// Forward declarations — these are PCSX2 globals
extern R5900DebugInterface r5900Debug;
extern R3000DebugInterface r3000Debug;

namespace DebugServer
{
	// ============================================================
	// Minimal JSON Builder
	// ============================================================
	class JsonBuilder
	{
	public:
		void startObject() { comma(); m_buf += '{'; m_needComma.push_back(false); }
		void endObject() { m_buf += '}'; m_needComma.pop_back(); if (!m_needComma.empty()) m_needComma.back() = true; }
		void startArray() { comma(); m_buf += '['; m_needComma.push_back(false); }
		void endArray() { m_buf += ']'; m_needComma.pop_back(); if (!m_needComma.empty()) m_needComma.back() = true; }

		void key(const char* k)
		{
			comma();
			m_buf += '"';
			escapeStr(k);
			m_buf += "\":";
			m_needComma.back() = false; // value follows
		}

		void valStr(const char* v) { comma(); m_buf += '"'; escapeStr(v); m_buf += '"'; m_needComma.back() = true; }
		void valStr(const std::string& v) { valStr(v.c_str()); }
		void valInt(int64_t v) { comma(); m_buf += std::to_string(v); m_needComma.back() = true; }
		void valUint(uint64_t v) { comma(); m_buf += std::to_string(v); m_needComma.back() = true; }
		void valBool(bool v) { comma(); m_buf += v ? "true" : "false"; m_needComma.back() = true; }
		void valNull() { comma(); m_buf += "null"; m_needComma.back() = true; }

		void valHex32(uint32_t v)
		{
			char buf[16];
			snprintf(buf, sizeof(buf), "0x%08x", v);
			comma();
			m_buf += '"';
			m_buf += buf;
			m_buf += '"';
			m_needComma.back() = true;
		}

		void valHex128(u128 v)
		{
			char buf[40];
			snprintf(buf, sizeof(buf), "%08x%08x%08x%08x",
				v._u32[3], v._u32[2], v._u32[1], v._u32[0]);
			comma();
			m_buf += '"';
			m_buf += buf;
			m_buf += '"';
			m_needComma.back() = true;
		}

		void valDouble(double v)
		{
			// snprintf("%.6g", ...) is locale-dependent - LC_NUMERIC can turn '.'
			// into ',' (e.g. under a de_DE locale), which silently produces invalid
			// JSON ("1,4e+06" instead of "1.4e+06"). std::to_chars is guaranteed
			// locale-independent and always round-trips correctly.
			char buf[64];
			auto result = std::to_chars(buf, buf + sizeof(buf), v);
			comma();
			m_buf.append(buf, result.ptr);
			m_needComma.back() = true;
		}

		// Key-value shortcuts
		void kv(const char* k, const char* v) { key(k); valStr(v); }
		void kv(const char* k, const std::string& v) { key(k); valStr(v); }
		void kv(const char* k, int v) { key(k); valInt(static_cast<int64_t>(v)); }
		void kv(const char* k, unsigned int v) { key(k); valUint(static_cast<uint64_t>(v)); }
		void kv(const char* k, int64_t v) { key(k); valInt(v); }
		void kv(const char* k, uint64_t v) { key(k); valUint(v); }
		void kv(const char* k, bool v) { key(k); valBool(v); }
		void kv(const char* k, double v) { key(k); valDouble(v); }

		std::string str() const { return m_buf; }
		void clear() { m_buf.clear(); m_needComma.clear(); }

	private:
		void comma()
		{
			if (!m_needComma.empty() && m_needComma.back())
				m_buf += ',';
		}
		void escapeStr(const char* s)
		{
			for (; *s; ++s)
			{
				switch (*s)
				{
					case '"': m_buf += "\\\""; break;
					case '\\': m_buf += "\\\\"; break;
					case '\n': m_buf += "\\n"; break;
					case '\r': m_buf += "\\r"; break;
					case '\t': m_buf += "\\t"; break;
					default: m_buf += *s; break;
				}
			}
		}

		std::string m_buf;
		std::vector<bool> m_needComma;
	};

	// ============================================================
	// Minimal JSON Parser (just enough for our commands)
	// ============================================================
	struct JsonValue
	{
		enum Type { NONE, STRING, NUMBER, BOOL, OBJECT };
		Type type = NONE;
		std::string strVal;
		int64_t numVal = 0;
		bool boolVal = false;
	};

	static std::unordered_map<std::string, JsonValue> parseJsonObject(const std::string& json)
	{
		std::unordered_map<std::string, JsonValue> result;
		size_t i = 0;
		// Skip to first '{'
		while (i < json.size() && json[i] != '{') i++;
		i++; // skip '{'

		while (i < json.size())
		{
			// Skip whitespace/commas
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ','))
				i++;
			if (i >= json.size() || json[i] == '}') break;

			// Read key
			if (json[i] != '"') break;
			i++;
			std::string key;
			while (i < json.size() && json[i] != '"')
			{
				if (json[i] == '\\' && i + 1 < json.size()) { key += json[i + 1]; i += 2; }
				else { key += json[i]; i++; }
			}
			i++; // skip closing '"'

			// Skip colon
			while (i < json.size() && json[i] != ':') i++;
			i++;

			// Skip whitespace
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;

			JsonValue val;
			if (json[i] == '"')
			{
				// String value
				i++;
				std::string sv;
				while (i < json.size() && json[i] != '"')
				{
					if (json[i] == '\\' && i + 1 < json.size()) { sv += json[i + 1]; i += 2; }
					else { sv += json[i]; i++; }
				}
				i++; // skip closing '"'
				val.type = JsonValue::STRING;
				val.strVal = sv;
			}
			else if (json[i] == 't' || json[i] == 'f')
			{
				val.type = JsonValue::BOOL;
				val.boolVal = (json[i] == 't');
				while (i < json.size() && json[i] != ',' && json[i] != '}') i++;
			}
			else if (json[i] == '-' || (json[i] >= '0' && json[i] <= '9'))
			{
				std::string ns;
				bool isHex = false;
				if (json[i] == '0' && i + 1 < json.size() && (json[i + 1] == 'x' || json[i + 1] == 'X'))
				{
					isHex = true;
					i += 2;
				}
				while (i < json.size() && ((json[i] >= '0' && json[i] <= '9') ||
					   json[i] == '-' ||
					   (isHex && ((json[i] >= 'a' && json[i] <= 'f') || (json[i] >= 'A' && json[i] <= 'F')))))
				{
					ns += json[i]; i++;
				}
				val.type = JsonValue::NUMBER;
				if (isHex) val.numVal = (int64_t)strtoull(ns.c_str(), nullptr, 16);
				else val.numVal = strtoll(ns.c_str(), nullptr, 10);
			}
			else
			{
				// Skip unknown
				while (i < json.size() && json[i] != ',' && json[i] != '}') i++;
			}

			result[key] = val;
		}

		return result;
	}

	static std::string getStr(const std::unordered_map<std::string, JsonValue>& m, const char* key, const char* def = "")
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::STRING) return it->second.strVal;
		return def;
	}

	static int64_t getNum(const std::unordered_map<std::string, JsonValue>& m, const char* key, int64_t def = 0)
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::NUMBER) return it->second.numVal;
		// Also try parsing string as hex
		if (it != m.end() && it->second.type == JsonValue::STRING)
		{
			const auto& s = it->second.strVal;
			if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
				return (int64_t)strtoull(s.c_str() + 2, nullptr, 16);
			return strtoll(s.c_str(), nullptr, 10);
		}
		return def;
	}

	static bool getBool(const std::unordered_map<std::string, JsonValue>& m, const char* key, bool def = false)
	{
		auto it = m.find(key);
		if (it != m.end() && it->second.type == JsonValue::BOOL) return it->second.boolVal;
		return def;
	}

	// Extracts a JSON array-of-objects value for one specific top-level key,
	// e.g. {"cmd":"read_memory_multiple","reads":[{"address":123,"length":4},...]}.
	// parseJsonObject() above only ever needed to handle a flat key:scalar
	// object - arrays were never a requirement until this command, so rather
	// than generalizing that parser (and risking every existing caller's
	// simpler behavior along the way), this is a narrow, purpose-built scanner:
	// find "key", find the matching [ ... ], then bracket-match each { ... }
	// inside it and hand each one off to parseJsonObject() individually.
	static std::vector<std::unordered_map<std::string, JsonValue>> parseJsonObjectArray(const std::string& json, const char* key)
	{
		std::vector<std::unordered_map<std::string, JsonValue>> result;

		std::string needle = std::string("\"") + key + "\"";
		size_t keyPos = json.find(needle);
		if (keyPos == std::string::npos)
			return result;

		size_t i = keyPos + needle.size();
		while (i < json.size() && json[i] != '[')
		{
			if (json[i] == '}') return result; // ran off the end of the object without finding a '['
			i++;
		}
		if (i >= json.size()) return result;
		i++; // skip '['

		while (i < json.size())
		{
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ','))
				i++;
			if (i >= json.size() || json[i] == ']') break;
			if (json[i] != '{') break;

			// Bracket-match this object, respecting quoted strings so a brace
			// inside a string value (e.g. a "description":"a } b" field) doesn't
			// confuse the scan.
			size_t start = i;
			int depth = 0;
			bool inString = false;
			for (; i < json.size(); i++)
			{
				char c = json[i];
				if (inString)
				{
					if (c == '\\') { i++; continue; }
					if (c == '"') inString = false;
					continue;
				}
				if (c == '"') { inString = true; continue; }
				if (c == '{') depth++;
				else if (c == '}')
				{
					depth--;
					if (depth == 0) { i++; break; }
				}
			}

			std::string obj = json.substr(start, i - start);
			result.push_back(parseJsonObject(obj));
		}

		return result;
	}

	// ============================================================
	// Get DebugInterface by CPU name
	// ============================================================
	static DebugInterface* getCpu(const std::string& name)
	{
		if (name == "iop" || name == "r3000" || name == "IOP")
			return &r3000Debug;
		return &r5900Debug; // default to EE
	}

	static BreakPointCpu getBpCpu(const std::string& name)
	{
		if (name == "iop" || name == "r3000" || name == "IOP")
			return BREAKPOINT_IOP;
		return BREAKPOINT_EE;
	}

	// ============================================================
	// Threading helpers
	// ============================================================
	// Every command handler used to run directly on its own raw client
	// thread. Register access (getRegister/setRegister/getPC/setPc/...)
	// and CBreakPoints (which has no internal locking) are also touched
	// by the CPU thread while it's emulating, so touching them from here
	// unmarshaled is a data race - stale reads, torn writes, or worse.
	// runOnCpuBlocking marshals a chunk of work onto the CPU thread and
	// blocks this (client) thread until it completes, so callers get a
	// consistent, race-free snapshot before building their JSON reply.
	static void runOnCpuBlocking(std::function<void()> fn)
	{
		Host::RunOnCPUThread(std::move(fn), true);
	}

	// Commands that touch CPU/memory/debugger state must gate on this
	// first: without it, a request that arrives before the VM has booted
	// (or after it has shut down) reads/writes stale or garbage state
	// instead of failing cleanly.
	static bool requireAlive(DebugInterface* cpu, JsonBuilder& j)
	{
		if (!cpu->isAlive())
		{
			j.startObject();
			j.kv("ok", false);
			j.kv("error", "VM is not running");
			j.endObject();
			return false;
		}
		return true;
	}

	// ============================================================
	// GS thread helpers
	// ============================================================
	// GS runs on its own dedicated thread ("MTGS") backed by a ring buffer -
	// a completely separate mechanism from Host::RunOnCPUThread. Unlike that
	// one, MTGS::RunOnGSThread() is fire-and-forget only (queues a pointer
	// packet and wakes the GS thread; no blocking variant). Reading GS state
	// for a single coherent JSON snapshot needs a wait-for-completion here,
	// so we build it ourselves with a plain mutex/condvar, executing the
	// read inside the queued callback exactly like runOnCpuBlocking does for
	// the CPU thread.
	static void runOnGsBlocking(std::function<void()> fn)
	{
		if (!MTGS::IsOpen())
		{
			// No GS thread running (e.g. VM not fully up yet) - nothing to
			// marshal onto, so just run inline rather than hang waiting for a
			// thread that will never pick up the queued call.
			fn();
			return;
		}

		std::mutex m;
		std::condition_variable cv;
		bool done = false;

		MTGS::RunOnGSThread([&]() {
			fn();
			{
				std::lock_guard<std::mutex> lock(m);
				done = true;
			}
			cv.notify_one();
		});

		std::unique_lock<std::mutex> lock(m);
		cv.wait(lock, [&] { return done; });
	}

	// Formats a raw 64-bit register value the same way every GIFRegXxx
	// union exposes it (a plain `.U64` member alongside the named
	// bitfields), so callers can always show "here's the exact bits"
	// alongside whichever named sub-fields we bothered to decode.
	static std::string hex64(u64 v)
	{
		char buf[20];
		snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)v);
		return buf;
	}

	// disVU0MicroUF/LF (DisVU0Micro.cpp) always prefix their returned text
	// with "%8.8x %8.8x:" (pc, code); disVU1MicroUF/LF (DisVU1Micro.cpp)
	// only do this when CpuVU1->IsInterpreter is true, and emit nothing
	// before the mnemonic otherwise - see the differing MakeDisF macros in
	// each file. We already report address/opcode as their own JSON fields,
	// so strip this prefix here rather than leaking PCSX2's current VU1
	// interpreter-vs-recompiler mode as an accidental formatting quirk into
	// our disassembly text.
	static std::string stripDisVuPrefix(const char* raw)
	{
		std::string s(raw ? raw : "");
		// Expected shape when present: 8 hex digits, space, 8 hex digits, colon.
		if (s.size() > 18 && s[8] == ' ' && s[17] == ':' &&
			std::all_of(s.begin(), s.begin() + 8, [](unsigned char c) { return std::isxdigit(c); }) &&
			std::all_of(s.begin() + 9, s.begin() + 17, [](unsigned char c) { return std::isxdigit(c); }))
		{
			s = s.substr(18);
		}
		// Trim leading spaces left over either way (both variants pad with a
		// leading space before the mnemonic, prefix or not).
		size_t start = s.find_first_not_of(' ');
		return start == std::string::npos ? std::string() : s.substr(start);
	}

	// ============================================================
	// Base64 (screenshot transport)
	// ============================================================
	// No base64 codec exists anywhere else in the PCSX2 tree (checked before
	// writing this - common/, 3rdparty, nothing). This is standard RFC 4648
	// base64 with padding, used only to get PNG/JPEG/WebP screenshot bytes
	// through the newline-delimited JSON-over-TCP protocol as a plain string.
	static std::string base64Encode(const u8* data, size_t len)
	{
		static constexpr char table[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		std::string out;
		out.reserve(((len + 2) / 3) * 4);

		size_t i = 0;
		for (; i + 2 < len; i += 3)
		{
			const u32 n = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8) | u32(data[i + 2]);
			out += table[(n >> 18) & 0x3F];
			out += table[(n >> 12) & 0x3F];
			out += table[(n >> 6) & 0x3F];
			out += table[n & 0x3F];
		}

		// Remaining 1 or 2 bytes need '=' padding to a 4-char group.
		const size_t remaining = len - i;
		if (remaining == 1)
		{
			const u32 n = u32(data[i]) << 16;
			out += table[(n >> 18) & 0x3F];
			out += table[(n >> 12) & 0x3F];
			out += "==";
		}
		else if (remaining == 2)
		{
			const u32 n = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8);
			out += table[(n >> 18) & 0x3F];
			out += table[(n >> 12) & 0x3F];
			out += table[(n >> 6) & 0x3F];
			out += '=';
		}

		return out;
	}

	// ============================================================
	// Memory search (Cheat-Engine-style value scanner)
	// ============================================================
	enum class MemValueType { U8, S8, U16, S16, U32, S32, U64, FLOAT };

	static MemValueType parseMemValueType(const std::string& s)
	{
		if (s == "u8") return MemValueType::U8;
		if (s == "s8") return MemValueType::S8;
		if (s == "u16") return MemValueType::U16;
		if (s == "s16") return MemValueType::S16;
		if (s == "s32") return MemValueType::S32;
		if (s == "u64") return MemValueType::U64;
		if (s == "float") return MemValueType::FLOAT;
		return MemValueType::U32; // default
	}

	static const char* memValueTypeName(MemValueType type)
	{
		switch (type)
		{
			case MemValueType::U8: return "u8";
			case MemValueType::S8: return "s8";
			case MemValueType::U16: return "u16";
			case MemValueType::S16: return "s16";
			case MemValueType::U32: return "u32";
			case MemValueType::S32: return "s32";
			case MemValueType::U64: return "u64";
			case MemValueType::FLOAT: return "float";
		}
		return "u32";
	}

	static int memValueTypeSize(MemValueType type)
	{
		switch (type)
		{
			case MemValueType::U8:
			case MemValueType::S8: return 1;
			case MemValueType::U16:
			case MemValueType::S16: return 2;
			case MemValueType::U32:
			case MemValueType::S32:
			case MemValueType::FLOAT: return 4;
			case MemValueType::U64: return 8;
		}
		return 4;
	}

	// Must be called already marshaled onto the CPU thread (via
	// runOnCpuBlocking) - same rule as every other cpu->Read* call here.
	static double readTypedValue(DebugInterface* cpu, u32 addr, MemValueType type, bool& valid)
	{
		valid = true;
		switch (type)
		{
			case MemValueType::U8: return (double)cpu->Read8(addr, &valid);
			case MemValueType::S8: return (double)(int8_t)cpu->Read8(addr, &valid);
			case MemValueType::U16: return (double)cpu->Read16(addr, &valid);
			case MemValueType::S16: return (double)(int16_t)cpu->Read16(addr, &valid);
			case MemValueType::U32: return (double)cpu->Read32(addr, &valid);
			case MemValueType::S32: return (double)(int32_t)cpu->Read32(addr, &valid);
			case MemValueType::U64: return (double)cpu->Read64(addr, &valid);
			case MemValueType::FLOAT:
			{
				u32 bits = cpu->Read32(addr, &valid);
				float f;
				std::memcpy(&f, &bits, sizeof(f));
				return (double)f;
			}
		}
		valid = false;
		return 0.0;
	}

	static bool compareValues(const std::string& comparison, double current, double reference, bool hasReference)
	{
		if (comparison == "equals") return hasReference && current == reference;
		if (comparison == "not_equals") return hasReference && current != reference;
		if (comparison == "greater") return hasReference && current > reference;
		if (comparison == "less") return hasReference && current < reference;
		if (comparison == "greater_equal") return hasReference && current >= reference;
		if (comparison == "less_equal") return hasReference && current <= reference;
		if (comparison == "changed") return current != reference;
		if (comparison == "unchanged") return current == reference;
		if (comparison == "increased") return current > reference;
		if (comparison == "decreased") return current < reference;
		return false;
	}

	struct MemSearchCandidate
	{
		u32 address;
		double value;
	};

	// A debugging tool like this is expected to be driven by one LLM/client
	// session at a time, so a single shared scan session (rather than one
	// per connection) keeps the protocol simple. Only ever touched from
	// clientHandler threads one command at a time, same as everything else
	// in this file.
	static bool s_searchActive = false;
	static MemValueType s_searchType = MemValueType::U32;
	static std::vector<MemSearchCandidate> s_searchCandidates;

	// ============================================================
	// GameDB / patch metadata helpers
	// ============================================================
	static const char* fpRoundModeName(FPRoundMode mode)
	{
		switch (mode)
		{
			case FPRoundMode::Nearest: return "nearest";
			case FPRoundMode::NegativeInfinity: return "negative_infinity";
			case FPRoundMode::PositiveInfinity: return "positive_infinity";
			case FPRoundMode::ChopZero: return "chop_zero";
			default: return "unset";
		}
	}

	static const char* clampModeName(GameDatabaseSchema::ClampMode mode)
	{
		switch (mode)
		{
			case GameDatabaseSchema::ClampMode::Disabled: return "disabled";
			case GameDatabaseSchema::ClampMode::Normal: return "normal";
			case GameDatabaseSchema::ClampMode::Extra: return "extra";
			case GameDatabaseSchema::ClampMode::Full: return "full";
			default: return "undefined";
		}
	}

	// ============================================================
	// Command Handlers
	// ============================================================

	static std::string handleCommand(const std::string& jsonLine)
	{
		auto params = parseJsonObject(jsonLine);
		std::string cmd = getStr(params, "cmd");
		std::string cpuName = getStr(params, "cpu", "ee");
		DebugInterface* cpu = getCpu(cpuName);

		JsonBuilder j;

		// ----- STATUS -----
		if (cmd == "status")
		{
			bool alive = cpu->isAlive();
			u32 pc = 0;
			int64_t cycles = 0;
			if (alive)
			{
				runOnCpuBlocking([&]() {
					pc = cpu->getPC();
					cycles = (int64_t)cpu->getCycles();
				});
			}

			j.startObject();
			j.kv("ok", true);
			j.key("data"); j.startObject();
			j.kv("alive", alive);
			j.kv("paused", cpu->isCpuPaused());
			j.key("pc"); j.valHex32(pc);
			j.kv("cycles", cycles);
			j.endObject();
			j.endObject();
		}
		// ----- READ REGISTERS (ALL) -----
		else if (cmd == "read_registers")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int cat = (int)getNum(params, "category", -1);
			int catCount = 0;
			runOnCpuBlocking([&]() { catCount = cpu->getRegisterCategoryCount(); });

			if (cat != -1 && (cat < 0 || cat >= catCount))
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "Invalid register category");
				j.endObject();
				return j.str();
			}

			j.startObject();
			j.kv("ok", true);
			j.key("data"); j.startObject();

			int catStart = (cat >= 0) ? cat : 0;
			int catEnd = (cat >= 0) ? cat + 1 : catCount;

			runOnCpuBlocking([&]() {
				for (int c = catStart; c < catEnd; c++)
				{
					j.key(cpu->getRegisterCategoryName(c));
					j.startObject();
					j.kv("size", cpu->getRegisterSize(c));
					j.kv("count", cpu->getRegisterCount(c));
					j.key("regs"); j.startArray();
					for (int r = 0; r < cpu->getRegisterCount(c); r++)
					{
						j.startObject();
						j.kv("name", cpu->getRegisterName(c, r));
						j.key("value"); j.valHex128(cpu->getRegister(c, r));
						j.kv("display", cpu->getRegisterString(c, r));
						j.endObject();
					}
					j.endArray();
					j.endObject();
				}

				j.key("pc"); j.valHex32(cpu->getPC());
				j.key("hi"); j.valHex128(cpu->getHI());
				j.key("lo"); j.valHex128(cpu->getLO());
			});
			j.endObject();
			j.endObject();
		}
		// ----- WRITE REGISTER -----
		else if (cmd == "write_register")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int cat = (int)getNum(params, "category", 0);
			int num = (int)getNum(params, "index", 0);

			int catCount = 0, regCount = 0;
			runOnCpuBlocking([&]() {
				catCount = cpu->getRegisterCategoryCount();
				if (cat >= 0 && cat < catCount)
					regCount = cpu->getRegisterCount(cat);
			});
			if (cat < 0 || cat >= catCount || num < 0 || num >= regCount)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "Invalid register category/index");
				j.endObject();
				return j.str();
			}

			// Value can be a hex string or number
			std::string valStr = getStr(params, "value", "0");
			// Parse hex string (up to 128-bit)
			if (valStr.size() > 2 && valStr[0] == '0' && (valStr[1] == 'x' || valStr[1] == 'X'))
				valStr = valStr.substr(2);
			// Keep only the low 32 hex chars (128 bits). Without this, a longer
			// string shifts the fixed-offset substr() slicing below out of
			// alignment and silently writes the wrong value.
			if (valStr.size() > 32)
				valStr = valStr.substr(valStr.size() - 32);
			// Pad to 32 hex chars (128 bits)
			while (valStr.size() < 32) valStr = "0" + valStr;

			u128 newVal = {};
			for (int i = 0; i < 4; i++)
			{
				std::string part = valStr.substr(24 - i * 8, 8);
				newVal._u32[i] = (uint32_t)strtoul(part.c_str(), nullptr, 16);
			}

			runOnCpuBlocking([&]() { cpu->setRegister(cat, num, newVal); });

			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- SET PC -----
		else if (cmd == "set_pc")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 pc = (u32)getNum(params, "value", 0);
			runOnCpuBlocking([&]() { cpu->setPc(pc); });
			j.startObject();
			j.kv("ok", true);
			j.key("pc"); j.valHex32(pc);
			j.endObject();
		}
		// ----- READ MEMORY -----
		else if (cmd == "read_memory")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 addr = (u32)getNum(params, "address", 0);
			int len = (int)getNum(params, "length", 256);
			if (len < 0) len = 0;
			if (len > 65536) len = 65536;

			j.startObject();
			j.kv("ok", true);
			j.key("address"); j.valHex32(addr);
			j.kv("length", (int64_t)len);

			// Hex string output
			j.key("hex");
			std::string hexStr;
			hexStr.reserve(len * 2);
			for (int i = 0; i < len; i++)
			{
				bool valid = true;
				u32 byte = cpu->Read8(addr + i, &valid);
				if (!valid) byte = 0;
				char hb[4];
				snprintf(hb, sizeof(hb), "%02x", byte & 0xFF);
				hexStr += hb;
			}
			j.valStr(hexStr);
			j.endObject();
		}
		// ----- WRITE MEMORY -----
		else if (cmd == "write_memory")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 addr = (u32)getNum(params, "address", 0);
			std::string hexData = getStr(params, "data", "");
			int written = 0;
			for (size_t i = 0; i + 1 < hexData.size(); i += 2)
			{
				u8 byte = (u8)strtoul(hexData.substr(i, 2).c_str(), nullptr, 16);
				cpu->Write8(addr + written, byte);
				written++;
			}
			j.startObject();
			j.kv("ok", true);
			j.kv("written", (int64_t)written);
			j.endObject();
		}
		// ----- READ MEMORY (MULTIPLE) -----
		// Mirrors the read_multiple_files pattern already used elsewhere in this
		// toolchain: one round-trip for a batch of {address,length} reads
		// instead of N separate read_memory calls. Each entry is independent -
		// a bad address in one doesn't fail the batch, it just comes back with
		// all_valid:false for that entry (matching read_memory's own permissive
		// per-byte validity handling above, surfaced at the whole-entry level
		// here since a per-byte breakdown isn't useful at batch scale).
		else if (cmd == "read_memory_multiple")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			auto reads = parseJsonObjectArray(jsonLine, "reads");

			j.startObject();
			j.kv("ok", true);
			j.key("results"); j.startArray();

			runOnCpuBlocking([&]() {
				for (auto& r : reads)
				{
					u32 addr = (u32)getNum(r, "address", 0);
					int len = (int)getNum(r, "length", 4);
					if (len < 0) len = 0;
					if (len > 65536) len = 65536;

					j.startObject();
					j.key("address"); j.valHex32(addr);
					j.kv("length", (int64_t)len);

					std::string hexStr;
					hexStr.reserve((size_t)len * 2);
					bool anyInvalid = false;
					for (int i = 0; i < len; i++)
					{
						bool valid = true;
						u32 byte = cpu->Read8(addr + i, &valid);
						if (!valid) { anyInvalid = true; byte = 0; }
						char hb[4];
						snprintf(hb, sizeof(hb), "%02x", byte & 0xFF);
						hexStr += hb;
					}
					j.kv("hex", hexStr);
					j.kv("all_valid", !anyInvalid);
					j.endObject();
				}
			});

			j.endArray();
			j.kv("count", (int64_t)reads.size());
			j.endObject();
		}
		// ----- DISASSEMBLE -----
		else if (cmd == "disassemble")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 addr = (u32)getNum(params, "address", 0);
			int count = (int)getNum(params, "count", 20);
			bool simplify = getBool(params, "simplify", true);
			if (count < 0) count = 0;
			if (count > 500) count = 500;

			j.startObject();
			j.kv("ok", true);
			j.key("instructions"); j.startArray();
			runOnCpuBlocking([&]() {
				for (int i = 0; i < count; i++)
				{
					u32 pc = addr + i * 4;
					if (!cpu->isValidAddress(pc)) break;

					bool valid = true;
					u32 opcode = cpu->Read32(pc, &valid);

					j.startObject();
					j.key("address"); j.valHex32(pc);
					j.key("opcode"); j.valHex32(opcode);
					j.kv("disasm", cpu->disasm(pc, simplify));
					j.endObject();
				}
			});
			j.endArray();
			j.endObject();
		}
		// ----- EVALUATE EXPRESSION -----
		else if (cmd == "evaluate")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			std::string expr = getStr(params, "expression", "0");
			u64 result = 0;
			std::string error;
			bool ok = false;
			runOnCpuBlocking([&]() { ok = cpu->evaluateExpression(expr.c_str(), result, error); });

			j.startObject();
			j.kv("ok", ok);
			if (ok)
			{
				j.key("result"); j.valUint(result);
				char hexBuf[20];
				snprintf(hexBuf, sizeof(hexBuf), "0x%llx", (unsigned long long)result);
				j.kv("hex", hexBuf);
			}
			else
			{
				j.kv("error", error);
			}
			j.endObject();
		}
		// ----- SET BREAKPOINT -----
		else if (cmd == "set_breakpoint")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			bool temp = getBool(params, "temporary", false);
			bool enabled = getBool(params, "enabled", true);
			std::string condExpr = getStr(params, "condition", "");
			std::string desc = getStr(params, "description", "");
			auto bpCpu = getBpCpu(cpuName);

			runOnCpuBlocking([&]() {
				CBreakPoints::AddBreakPoint(bpCpu, addr, temp, enabled);

				if (!desc.empty())
					CBreakPoints::ChangeBreakPointDescription(bpCpu, addr, desc);

				if (!condExpr.empty())
				{
					BreakPointCond cond;
					cond.debug = cpu;
					cond.expressionString = condExpr;
					std::string error;
					if (cpu->initExpression(condExpr.c_str(), cond.expression, error))
					{
						CBreakPoints::ChangeBreakPointAddCond(bpCpu, addr, cond);
					}
				}
			});

			j.startObject();
			j.kv("ok", true);
			j.key("address"); j.valHex32(addr);
			j.endObject();
		}
		// ----- REMOVE BREAKPOINT -----
		else if (cmd == "remove_breakpoint")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			auto bpCpu = getBpCpu(cpuName);
			runOnCpuBlocking([&]() { CBreakPoints::RemoveBreakPoint(bpCpu, addr); });
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- SET MEMCHECK (WATCHPOINT) -----
		else if (cmd == "set_memcheck")
		{
			u32 start = (u32)getNum(params, "address", 0);
			u32 end = (u32)getNum(params, "end", start + 4);
			std::string typeStr = getStr(params, "type", "write");
			std::string actionStr = getStr(params, "action", "break");
			std::string desc = getStr(params, "description", "");
			std::string condExpr = getStr(params, "condition", "");

			MemCheckCondition cond = MEMCHECK_WRITE;
			if (typeStr == "read") cond = MEMCHECK_READ;
			else if (typeStr == "readwrite" || typeStr == "access") cond = MEMCHECK_READWRITE;
			else if (typeStr == "onchange") cond = (MemCheckCondition)(MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE);

			MemCheckResult result = MEMCHECK_BREAK;
			if (actionStr == "log") result = MEMCHECK_LOG;
			else if (actionStr == "both") result = MEMCHECK_BOTH;

			auto bpCpu = getBpCpu(cpuName);

			runOnCpuBlocking([&]() {
				CBreakPoints::AddMemCheck(bpCpu, start, end, cond, result);

				if (!desc.empty())
					CBreakPoints::ChangeMemCheckDescription(bpCpu, start, end, desc);

				if (!condExpr.empty())
				{
					BreakPointCond bpCond;
					bpCond.debug = cpu;
					bpCond.expressionString = condExpr;
					std::string error;
					if (cpu->initExpression(condExpr.c_str(), bpCond.expression, error))
						CBreakPoints::ChangeMemCheckAddCond(bpCpu, start, end, bpCond);
				}
			});

			j.startObject();
			j.kv("ok", true);
			j.key("start"); j.valHex32(start);
			j.key("end"); j.valHex32(end);
			j.endObject();
		}
		// ----- REMOVE MEMCHECK -----
		else if (cmd == "remove_memcheck")
		{
			u32 start = (u32)getNum(params, "address", 0);
			u32 end = (u32)getNum(params, "end", start + 4);
			auto bpCpu = getBpCpu(cpuName);
			runOnCpuBlocking([&]() { CBreakPoints::RemoveMemCheck(bpCpu, start, end); });
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- LIST BREAKPOINTS -----
		else if (cmd == "list_breakpoints")
		{
			auto bpCpu = getBpCpu(cpuName);
			j.startObject();
			j.kv("ok", true);
			j.key("breakpoints"); j.startArray();
			runOnCpuBlocking([&]() {
				auto bps = CBreakPoints::GetBreakpoints(bpCpu, true);
				for (const auto& bp : bps)
				{
					j.startObject();
					j.key("address"); j.valHex32(bp.addr);
					j.kv("enabled", bp.enabled);
					j.kv("temporary", bp.temporary);
					j.kv("stepping", bp.stepping);
					j.kv("has_condition", bp.hasCond);
					if (bp.hasCond)
						j.kv("condition", bp.cond.expressionString);
					if (!bp.description.empty())
						j.kv("description", bp.description);
					j.endObject();
				}
			});
			j.endArray();
			j.endObject();
		}
		// ----- LIST MEMCHECKS -----
		else if (cmd == "list_memchecks")
		{
			auto bpCpu = getBpCpu(cpuName);
			j.startObject();
			j.kv("ok", true);
			j.key("memchecks"); j.startArray();
			runOnCpuBlocking([&]() {
				auto mcs = CBreakPoints::GetMemChecks(bpCpu);
				for (const auto& mc : mcs)
				{
					j.startObject();
					j.key("start"); j.valHex32(mc.start);
					j.key("end"); j.valHex32(mc.end);
					j.kv("hits", (int64_t)mc.numHits);
					j.key("last_pc"); j.valHex32(mc.lastPC);
					j.key("last_addr"); j.valHex32(mc.lastAddr);
					if (!mc.description.empty())
						j.kv("description", mc.description);
					j.endObject();
				}
			});
			j.endArray();
			j.endObject();
		}
		// ----- PAUSE -----
		else if (cmd == "pause")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 pc = 0;
			runOnCpuBlocking([&]() {
				cpu->pauseCpu();
				pc = cpu->getPC();
			});
			j.startObject();
			j.kv("ok", true);
			j.kv("paused", cpu->isCpuPaused());
			j.key("pc"); j.valHex32(pc);
			j.endObject();
		}
		// ----- RESUME -----
		else if (cmd == "resume")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			auto bpCpu = getBpCpu(cpuName);
			runOnCpuBlocking([&]() {
				// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
				CBreakPoints::SetSkipFirst(bpCpu, cpu->getPC());
				cpu->resumeCpu();
			});
			j.startObject();
			j.kv("ok", true);
			// resume is fire-and-forget by design (unlike resume_and_wait below) -
			// this almost always reads as false immediately after, but it's
			// included for consistency so every response is self-describing
			// about pause state rather than requiring a follow-up status call.
			j.kv("paused", cpu->isCpuPaused());
			j.endObject();
		}
		// ----- RESUME AND WAIT FOR NEXT PAUSE -----
		// Fills a real gap: a plain "resume" after setting a breakpoint doesn't
		// tell the caller *when* (or whether) that breakpoint fires - the only
		// option otherwise is polling "status" in a loop. This blocks until the
		// VM re-pauses (breakpoint/watchpoint hit, or someone else paused it)
		// or the timeout elapses, mirroring how step/step_over already work.
		else if (cmd == "resume_and_wait")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int timeoutMs = (int)getNum(params, "timeout_ms", 10000);
			if (timeoutMs < 0) timeoutMs = 0;
			if (timeoutMs > 60000) timeoutMs = 60000;

			auto bpCpu = getBpCpu(cpuName);
			runOnCpuBlocking([&]() {
				CBreakPoints::SetSkipFirst(bpCpu, cpu->getPC());
				cpu->resumeCpu();
			});

			int timeout = timeoutMs;
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				timeout -= 2;
			}

			u32 pc = 0;
			runOnCpuBlocking([&]() { pc = cpu->getPC(); });

			j.startObject();
			j.kv("ok", true);
			j.kv("timed_out", !cpu->isCpuPaused());
			j.kv("paused", cpu->isCpuPaused());
			j.key("pc"); j.valHex32(pc);
			j.endObject();
		}
		// ----- STEP -----
		else if (cmd == "step")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			auto bpCpu = getBpCpu(cpuName);

			// Setup runs marshaled on the CPU thread: reading pc and touching
			// CBreakPoints/resumeCpu must not race the CPU thread's own use of
			// that state. This must stay a short, non-blocking-for-the-emu-loop
			// operation - it must NOT include the wait loop below, since that
			// would occupy the CPU thread and prevent it from ever actually
			// stepping forward (deadlocking against ourselves).
			u32 pc = 0;
			u32 nextPc = 0;
			runOnCpuBlocking([&]() {
				pc = cpu->getPC();
				// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
				CBreakPoints::SetSkipFirst(bpCpu, pc);

				nextPc = pc + 4;
				CBreakPoints::AddBreakPoint(bpCpu, nextPc, true, true, true);
				cpu->resumeCpu();
			});

			// Wait for it to hit (with timeout). isCpuPaused() just reads the
			// VM state flag (same as VMManager::GetState() polled elsewhere in
			// the codebase), so it's safe to poll from this thread without
			// marshaling onto the CPU thread.
			int timeout = 5000; // ms
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				timeout--;
			}

			bool timedOut = !cpu->isCpuPaused();
			// Whether it hit or timed out, this breakpoint has served its purpose
			// and CBreakPoints does not auto-remove "temporary" breakpoints on
			// hit - it just means "don't show it as a user-set one". Leaving it
			// in place means the next step/step_over call can find itself sitting
			// exactly on a stale breakpoint from a prior call, which prevents
			// SetSkipFirst from letting execution actually advance.
			runOnCpuBlocking([&]() { CBreakPoints::RemoveBreakPoint(bpCpu, nextPc); });

			u32 newPc = 0;
			std::string disasm;
			u32 opcode = 0;
			bool inBios = false;
			runOnCpuBlocking([&]() {
				newPc = cpu->getPC();
				inBios = (newPc < 0x00100000) || (newPc >= 0x80000000 && newPc < 0x80100000);
				disasm = cpu->disasm(newPc, true);
				bool valid = true;
				opcode = cpu->Read32(newPc, &valid);
			});

			j.startObject();
			j.kv("ok", true);
			j.kv("timed_out", timedOut);
			j.kv("paused", cpu->isCpuPaused());
			j.key("old_pc"); j.valHex32(pc);
			j.key("new_pc"); j.valHex32(newPc);
			j.kv("disasm", disasm);
			j.kv("in_bios", inBios);
			j.key("opcode"); j.valHex32(opcode);
			j.endObject();
		}
		// ----- STEP OVER -----
		else if (cmd == "step_over")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			auto bpCpu = getBpCpu(cpuName);

			// See the comment on "step" above: setup is marshaled and short,
			// the wait loop below is not, to avoid deadlocking the CPU thread
			// against its own progress.
			u32 pc = 0;
			u32 bpAddr = 0;
			runOnCpuBlocking([&]() {
				pc = cpu->getPC();
				// Skip current BP if we're sitting on one (matches PCSX2 GUI behavior)
				CBreakPoints::SetSkipFirst(bpCpu, pc);

				bool valid = true;
				u32 opcode = cpu->Read32(pc, &valid);
				u32 op = (opcode >> 26) & 63;

				if (op == 3 || // JAL
					(op == 0 && (opcode & 63) == 9)) // JALR
				{
					bpAddr = pc + 8;
				}
				else
				{
					bpAddr = pc + 4;
				}

				CBreakPoints::AddBreakPoint(bpCpu, bpAddr, true, true, true);
				cpu->resumeCpu();
			});

			int timeout = 10000;
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				timeout--;
			}

			bool timedOut = !cpu->isCpuPaused();
			// See the comment in "step" above: always clean up, not just on timeout.
			runOnCpuBlocking([&]() { CBreakPoints::RemoveBreakPoint(bpCpu, bpAddr); });

			u32 newPc = 0;
			std::string disasm;
			bool inBios = false;
			runOnCpuBlocking([&]() {
				newPc = cpu->getPC();
				inBios = (newPc < 0x00100000) || (newPc >= 0x80000000 && newPc < 0x80100000);
				disasm = cpu->disasm(newPc, true);
			});

			j.startObject();
			j.kv("ok", true);
			j.kv("timed_out", timedOut);
			j.kv("paused", cpu->isCpuPaused());
			j.key("old_pc"); j.valHex32(pc);
			j.key("new_pc"); j.valHex32(newPc);
			j.kv("disasm", disasm);
			j.kv("in_bios", inBios);
			j.endObject();
		}
		// ----- GET THREADS -----
		else if (cmd == "get_threads")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			j.startObject();
			j.kv("ok", true);
			j.key("threads"); j.startArray();
			runOnCpuBlocking([&]() {
				auto threads = cpu->GetThreadList();
				for (const auto& t : threads)
				{
					j.startObject();
					j.kv("id", (int64_t)t->TID());
					j.key("pc"); j.valHex32(t->PC());
					j.kv("status", (int64_t)(int)t->Status());
					j.kv("wait_type", (int64_t)(int)t->Wait());
					j.endObject();
				}
			});
			j.endArray();
			j.endObject();
		}
		// ----- GET MODULES (IOP only) -----
		else if (cmd == "get_modules")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			j.startObject();
			j.kv("ok", true);
			j.key("modules"); j.startArray();
			runOnCpuBlocking([&]() {
				auto mods = cpu->GetModuleList();
				for (const auto& m : mods)
				{
					j.startObject();
					j.kv("name", m.name);
					j.kv("version", (int64_t)m.version);
					j.endObject();
				}
			});
			j.endArray();
			j.endObject();
		}
		// ----- GET BACKTRACE -----
		else if (cmd == "get_backtrace")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int maxFrames = (int)getNum(params, "max_frames", 32);
			if (maxFrames < 0) maxFrames = 0;
			if (maxFrames > 128) maxFrames = 128;

			j.startObject();
			j.kv("ok", true);
			j.key("frames"); j.startArray();
			int64_t frameCount = 0;
			runOnCpuBlocking([&]() {
				u32 pc = cpu->getPC();
				u32 ra = cpu->getRegister(0, 31); // $ra
				u32 sp = cpu->getRegister(0, 29); // $sp

				// Find the running thread to get entry point
				u32 threadEntry = 0;
				auto threads = cpu->GetThreadList();
				for (const auto& t : threads)
				{
					if (t->Status() == ThreadStatus::THS_RUN)
					{
						threadEntry = t->EntryPoint();
						break;
					}
				}

				auto frames = MipsStackWalk::Walk(cpu, pc, ra, sp, threadEntry);
				frameCount = (int64_t)(std::min)((int)frames.size(), maxFrames);

				int count = 0;
				for (const auto& f : frames)
				{
					if (count >= maxFrames) break;
					j.startObject();
					j.key("entry"); j.valHex32(f.entry);
					j.key("pc"); j.valHex32(f.pc);
					j.key("sp"); j.valHex32(f.sp);
					j.kv("stack_size", (int64_t)f.stackSize);
					j.kv("disasm", cpu->disasm(f.pc, true));
					j.endObject();
					count++;
				}
			});
			j.endArray();
			j.kv("frame_count", frameCount);
			j.endObject();
		}
		// ----- IS VALID ADDRESS -----
		else if (cmd == "is_valid_address")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 addr = (u32)getNum(params, "address", 0);
			bool valid = false;
			runOnCpuBlocking([&]() { valid = cpu->isValidAddress(addr); });
			j.startObject();
			j.kv("ok", true);
			j.kv("valid", valid);
			j.endObject();
		}
		// ----- READ STRING -----
		else if (cmd == "read_string")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 addr = (u32)getNum(params, "address", 0);
			int maxLen = (int)getNum(params, "max_length", 256);
			if (maxLen < 0) maxLen = 0;
			if (maxLen > 4096) maxLen = 4096;

			std::string str;
			for (int i = 0; i < maxLen; i++)
			{
				bool valid = true;
				u32 byte = cpu->Read8(addr + i, &valid);
				if (!valid || byte == 0) break;
				str += (char)byte;
			}

			j.startObject();
			j.kv("ok", true);
			j.kv("string", str);
			j.kv("length", (int64_t)str.size());
			j.endObject();
		}
		// ----- CLEAR ALL BREAKPOINTS -----
		else if (cmd == "clear_breakpoints")
		{
			runOnCpuBlocking([&]() {
				CBreakPoints::ClearAllBreakPoints();
				CBreakPoints::ClearAllMemChecks();
			});
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ============================================================
		// Symbol intelligence (SymbolGuardian). These are deliberately NOT
		// gated on requireAlive() or marshaled through runOnCpuBlocking:
		// SymbolGuardian keeps its own internal shared_mutex (see
		// SymbolGuardian.h) so it's already safe to touch from any thread,
		// and the symbol database can be meaningful even before a VM has
		// booted (e.g. symbols loaded from an ELF for pre-analysis).
		// ============================================================
		// ----- RESOLVE ADDRESS -----
		else if (cmd == "resolve_address")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			SymbolInfo info = cpu->GetSymbolGuardian().SymbolOverlappingAddress(addr);

			j.startObject();
			j.kv("ok", true);
			bool found = info.address.valid();
			j.kv("found", found);
			if (found)
			{
				j.kv("name", info.name);
				j.key("symbol_address"); j.valHex32(info.address.value);
				j.kv("size", info.size);
				j.kv("offset", (int64_t)(addr - info.address.value));
			}
			j.endObject();
		}
		// ----- FIND SYMBOL -----
		else if (cmd == "find_symbol")
		{
			std::string name = getStr(params, "name", "");
			SymbolInfo info = cpu->GetSymbolGuardian().SymbolWithName(name);

			j.startObject();
			j.kv("ok", true);
			bool found = info.address.valid();
			j.kv("found", found);
			if (found)
			{
				j.key("address"); j.valHex32(info.address.value);
				j.kv("size", info.size);
			}
			j.endObject();
		}
		// ----- GET FUNCTION INFO -----
		else if (cmd == "get_function_info")
		{
			u32 addr = (u32)getNum(params, "address", 0);
			FunctionInfo info = cpu->GetSymbolGuardian().FunctionOverlappingAddress(addr);

			j.startObject();
			j.kv("ok", true);
			bool found = info.address.valid();
			j.kv("found", found);
			if (found)
			{
				j.kv("name", info.name);
				j.key("address"); j.valHex32(info.address.value);
				j.kv("size", info.size);
				j.kv("is_no_return", info.is_no_return);
			}
			j.endObject();
		}
		// ----- LIST FUNCTIONS -----
		else if (cmd == "list_functions")
		{
			u32 start = (u32)getNum(params, "start", 0);
			u32 end = (u32)getNum(params, "end", 0xFFFFFFFF);
			int limit = (int)getNum(params, "limit", 500);
			if (limit < 0) limit = 0;
			if (limit > 5000) limit = 5000;

			j.startObject();
			j.kv("ok", true);
			j.key("functions"); j.startArray();
			int64_t total = 0;
			cpu->GetSymbolGuardian().Read([&](const ccc::SymbolDatabase& db) {
				int count = 0;
				for (const ccc::Function& function : db.functions)
				{
					if (!function.address().valid()) continue;
					u32 addr = function.address().value;
					if (addr < start || addr > end) continue;

					total++;
					if (count >= limit) continue;
					count++;

					j.startObject();
					j.kv("name", function.name());
					j.key("address"); j.valHex32(addr);
					j.kv("size", function.size());
					j.kv("original_hash", (int64_t)function.original_hash());
					j.kv("current_hash", (int64_t)function.current_hash());
					j.endObject();
				}
			});
			j.endArray();
			j.kv("total_count", total);
			j.endObject();
		}
		// ============================================================
		// Code analysis
		// ============================================================
		// ----- DECODE INSTRUCTION -----
		else if (cmd == "decode_instruction")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 addr = (u32)getNum(params, "address", 0);
			MIPSAnalyst::MipsOpcodeInfo info{};
			// GetOpcodeInfo reads live register values (to resolve branch
			// targets/conditions), so this needs the same CPU-thread marshaling
			// as register reads elsewhere in this file.
			runOnCpuBlocking([&]() { info = MIPSAnalyst::GetOpcodeInfo(cpu, addr); });

			j.startObject();
			j.kv("ok", true);
			j.key("address"); j.valHex32(addr);
			j.key("opcode"); j.valHex32(info.encodedOpcode);
			j.kv("is_branch", info.isBranch);
			j.kv("is_conditional", info.isConditional);
			j.kv("condition_met", info.conditionMet);
			j.key("branch_target"); j.valHex32(info.branchTarget);
			j.kv("is_linked_branch", info.isLinkedBranch);
			j.kv("is_likely_branch", info.isLikelyBranch);
			j.kv("is_branch_to_register", info.isBranchToRegister);
			j.kv("branch_register_num", info.branchRegisterNum);
			j.kv("is_syscall", info.isSyscall);
			j.kv("is_data_access", info.isDataAccess);
			j.kv("data_size", info.dataSize);
			j.key("data_address"); j.valHex32(info.dataAddress);
			j.kv("has_relevant_address", info.hasRelevantAddress);
			j.key("relevant_address"); j.valHex32(info.releventAddress);
			j.endObject();
		}
		// ----- ASSEMBLE -----
		else if (cmd == "assemble")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 addr = (u32)getNum(params, "address", 0);
			std::string text = getStr(params, "instruction", "");

			u32 encoded = 0;
			std::string error;
			bool ok = false;
			runOnCpuBlocking([&]() { ok = MipsAssembleOpcode(text.c_str(), cpu, addr, encoded, error); });

			j.startObject();
			j.kv("ok", ok);
			if (ok)
			{
				j.key("opcode"); j.valHex32(encoded);
			}
			else
			{
				j.kv("error", error);
			}
			j.endObject();
		}
		// ----- SCAN FOR FUNCTIONS -----
		else if (cmd == "scan_functions")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 start = (u32)getNum(params, "start", 0);
			u32 end = (u32)getNum(params, "end", 0);
			bool generateHashes = getBool(params, "generate_hashes", true);

			if (end <= start)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "end must be greater than start");
				j.endObject();
				return j.str();
			}

			int64_t countBefore = 0, countAfter = 0;
			runOnCpuBlocking([&]() {
				cpu->GetSymbolGuardian().Read([&](const ccc::SymbolDatabase& db) { countBefore = db.functions.size(); });
				cpu->GetSymbolGuardian().ReadWrite([&](ccc::SymbolDatabase& db) {
					MIPSAnalyst::ScanForFunctions(db, *cpu, start, end, generateHashes);
				});
				cpu->GetSymbolGuardian().Read([&](const ccc::SymbolDatabase& db) { countAfter = db.functions.size(); });
			});

			j.startObject();
			j.kv("ok", true);
			j.kv("functions_before", countBefore);
			j.kv("functions_after", countAfter);
			j.kv("functions_added", countAfter - countBefore);
			j.endObject();
		}
		// ============================================================
		// Memory search (Cheat-Engine-style value scanner)
		// ============================================================
		// ----- SEARCH MEMORY: START -----
		else if (cmd == "search_memory_start")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			u32 start = (u32)getNum(params, "start", 0);
			u32 end = (u32)getNum(params, "end", start + 0x2000000); // default: 32MB span
			std::string typeStr = getStr(params, "type", "u32");
			std::string comparison = getStr(params, "comparison", "");
			bool hasValue = params.count("value") > 0;
			double value = hasValue ? (double)getNum(params, "value", 0) : 0.0;

			MemValueType type = parseMemValueType(typeStr);
			int size = memValueTypeSize(type);

			if (end <= start)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "end must be greater than start");
				j.endObject();
				return j.str();
			}

			s_searchCandidates.clear();
			s_searchType = type;
			s_searchActive = true;

			runOnCpuBlocking([&]() {
				for (u32 addr = start; addr + (u32)size <= end; addr += (u32)size)
				{
					bool valid = true;
					double v = readTypedValue(cpu, addr, type, valid);
					if (!valid) continue;
					if (comparison.empty() || compareValues(comparison, v, value, hasValue))
						s_searchCandidates.push_back({addr, v});
				}
			});

			j.startObject();
			j.kv("ok", true);
			j.kv("type", memValueTypeName(type));
			j.kv("candidate_count", (int64_t)s_searchCandidates.size());
			j.endObject();
		}
		// ----- SEARCH MEMORY: NEXT (narrow candidates) -----
		else if (cmd == "search_memory_next")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			if (!s_searchActive)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "No active search - call search_memory_start first");
				j.endObject();
				return j.str();
			}

			std::string comparison = getStr(params, "comparison", "changed");
			bool hasValue = params.count("value") > 0;
			double value = hasValue ? (double)getNum(params, "value", 0) : 0.0;

			runOnCpuBlocking([&]() {
				std::vector<MemSearchCandidate> next;
				next.reserve(s_searchCandidates.size());
				for (auto& c : s_searchCandidates)
				{
					bool valid = true;
					double v = readTypedValue(cpu, c.address, s_searchType, valid);
					if (!valid) continue;
					double reference = hasValue ? value : c.value;
					if (compareValues(comparison, v, reference, true))
						next.push_back({c.address, v});
				}
				s_searchCandidates = std::move(next);
			});

			j.startObject();
			j.kv("ok", true);
			j.kv("candidate_count", (int64_t)s_searchCandidates.size());
			j.endObject();
		}
		// ----- SEARCH MEMORY: RESULTS -----
		else if (cmd == "search_memory_results")
		{
			int limit = (int)getNum(params, "limit", 50);
			if (limit < 0) limit = 0;
			if (limit > 1000) limit = 1000;

			j.startObject();
			j.kv("ok", true);
			j.kv("type", memValueTypeName(s_searchType));
			j.kv("candidate_count", (int64_t)s_searchCandidates.size());
			j.key("results"); j.startArray();
			for (int i = 0; i < (int)s_searchCandidates.size() && i < limit; i++)
			{
				j.startObject();
				j.key("address"); j.valHex32(s_searchCandidates[i].address);
				j.kv("value", s_searchCandidates[i].value);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ----- SEARCH MEMORY: RESET -----
		else if (cmd == "search_memory_reset")
		{
			s_searchCandidates.clear();
			s_searchActive = false;
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ============================================================
		// VM / session control
		// ============================================================
		// ----- GET GAME INFO -----
		else if (cmd == "get_game_info")
		{
			bool alive = cpu->isAlive();
			std::string title, serial, elf;
			u32 crc = 0;
			if (alive)
			{
				runOnCpuBlocking([&]() {
					title = VMManager::GetTitle(true);
					serial = VMManager::GetDiscSerial();
					elf = VMManager::GetDiscELF();
					crc = VMManager::GetDiscCRC();
				});
			}

			j.startObject();
			j.kv("ok", true);
			j.kv("alive", alive);
			if (alive)
			{
				j.kv("title", title);
				j.kv("serial", serial);
				j.kv("elf", elf);
				char crcBuf[16];
				snprintf(crcBuf, sizeof(crcBuf), "0x%08x", crc);
				j.kv("crc", crcBuf);
			}
			j.endObject();
		}
		// ----- RESET VM -----
		else if (cmd == "reset_vm")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			bool success = false;
			runOnCpuBlocking([&]() { success = VMManager::RequestReset(); });

			// VMManager::Reset() only resets synchronously if the VM was already
			// paused. If it was running, Reset() just flips state to Resetting
			// and returns immediately - the actual reset happens later, on the
			// main loop. Without waiting here, "ok:true" would mean "the reset
			// was scheduled", not "the reset happened", which is a dangerously
			// easy distinction to miss.
			if (success)
			{
				int timeout = 10000;
				while (VMManager::GetState() == VMState::Resetting && timeout > 0)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
					timeout -= 2;
				}
			}

			j.startObject();
			j.kv("ok", success);
			if (!success)
				j.kv("error", "Reset was blocked (e.g. memory card busy) - try again shortly");
			j.kv("paused", cpu->isAlive() && cpu->isCpuPaused());
			j.endObject();
		}
		// ----- FRAME ADVANCE -----
		else if (cmd == "frame_advance")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int frames = (int)getNum(params, "frames", 1);
			if (frames < 1) frames = 1;
			if (frames > 3600) frames = 3600;

			runOnCpuBlocking([&]() { VMManager::FrameAdvance((u32)frames); });

			// VMManager::FrameAdvance() is fire-and-forget: it just sets a frame
			// counter and unpauses, then the main loop re-pauses once that many
			// frames have actually run. Without waiting for that, this command
			// would return immediately while the VM is still mid-flight -
			// surprising for a command whose whole point is synchronous,
			// deterministic stepping. isCpuPaused() is a safe unmarshaled poll,
			// same reasoning as the step/step_over wait loops above.
			int timeout = frames * 2000 + 2000; // generous per-frame allowance
			if (timeout > 30000) timeout = 30000;
			while (!cpu->isCpuPaused() && timeout > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				timeout -= 2;
			}
			bool timedOut = !cpu->isCpuPaused();

			u32 pc = 0;
			runOnCpuBlocking([&]() { pc = cpu->getPC(); });

			j.startObject();
			j.kv("ok", true);
			j.kv("timed_out", timedOut);
			j.kv("paused", cpu->isCpuPaused());
			j.kv("frames", (int64_t)frames);
			j.key("pc"); j.valHex32(pc);
			j.endObject();
		}
		// ----- SAVE STATE TO SLOT -----
		else if (cmd == "save_state_slot")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int slot = (int)getNum(params, "slot", 0);

			std::string errorMsg;
			runOnCpuBlocking([&]() {
				// zip_on_thread=false: we need this to be synchronous so the error
				// callback (which captures our local errorMsg by reference) always
				// fires before this lambda - and therefore this whole blocking call
				// - returns. With zip_on_thread=true the compression happens on a
				// separate thread and the callback fires later, well after errorMsg
				// has gone out of scope.
				VMManager::SaveStateToSlot(slot, false, [&errorMsg](const std::string& error) { errorMsg = error; });
			});

			j.startObject();
			j.kv("ok", errorMsg.empty());
			if (!errorMsg.empty())
				j.kv("error", errorMsg);
			j.kv("slot", (int64_t)slot);
			// Saving never changes whether the VM is paused/running - noted here
			// explicitly (rather than left implicit) since load_state_slot right
			// below has the same property and it's easy to assume otherwise.
			j.kv("paused", cpu->isCpuPaused());
			j.endObject();
		}
		// ----- LOAD STATE FROM SLOT -----
		else if (cmd == "load_state_slot")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int slot = (int)getNum(params, "slot", 0);
			bool backup = getBool(params, "backup", false);

			bool success = false;
			Error error;
			runOnCpuBlocking([&]() { success = VMManager::LoadStateFromSlot(slot, backup, &error); });

			j.startObject();
			j.kv("ok", success);
			if (!success)
				j.kv("error", error.GetDescription());
			j.kv("slot", (int64_t)slot);
			// Important: loading a state does NOT restore whatever pause state was
			// active when the state was saved - the save format doesn't carry that
			// at all. It only overwrites CPU/memory/GS state, so paused/running
			// here just reflects whatever the VM was doing right before this call.
			j.kv("paused", cpu->isCpuPaused());
			j.endObject();
		}
		// ----- CHANGE DISC -----
		else if (cmd == "change_disc")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			std::string path = getStr(params, "path", "");
			std::string sourceStr = getStr(params, "source", "iso");

			CDVD_SourceType source = CDVD_SourceType::Iso;
			if (sourceStr == "disc") source = CDVD_SourceType::Disc;
			else if (sourceStr == "nodisc") source = CDVD_SourceType::NoDisc;

			bool success = false;
			runOnCpuBlocking([&]() { success = VMManager::ChangeDisc(source, path); });

			j.startObject();
			j.kv("ok", success);
			if (!success)
				j.kv("error", "ChangeDisc failed - check the path is valid");
			j.kv("paused", cpu->isCpuPaused());
			j.endObject();
		}
		// ============================================================
		// GameDB / patch metadata (static per-game data - no requireAlive()
		// or CPU-thread marshaling needed for the lookups themselves, same
		// exception as SymbolGuardian; only the serial/CRC read below touches
		// VMManager state and is marshaled for consistency with get_game_info).
		// ============================================================
		// ----- GET GAMEDB INFO -----
		else if (cmd == "get_gamedb_info")
		{
			bool alive = cpu->isAlive();
			std::string serial;
			u32 crc = 0;
			if (alive)
			{
				runOnCpuBlocking([&]() {
					serial = VMManager::GetDiscSerial();
					crc = VMManager::GetDiscCRC();
				});
			}

			j.startObject();
			j.kv("ok", true);
			j.kv("alive", alive);

			const GameDatabaseSchema::GameEntry* entry = (alive && !serial.empty()) ? GameDatabase::findGame(serial) : nullptr;
			j.kv("found", entry != nullptr);

			if (entry)
			{
				j.kv("name", entry->name);
				j.kv("name_en", entry->name_en);
				j.kv("region", entry->region);
				j.kv("compatibility", entry->compatAsString());
				j.kv("ee_round_mode", fpRoundModeName(entry->eeRoundMode));
				j.kv("ee_div_round_mode", fpRoundModeName(entry->eeDivRoundMode));
				j.kv("vu0_round_mode", fpRoundModeName(entry->vu0RoundMode));
				j.kv("vu1_round_mode", fpRoundModeName(entry->vu1RoundMode));
				j.kv("ee_clamp_mode", clampModeName(entry->eeClampMode));
				j.kv("vu0_clamp_mode", clampModeName(entry->vu0ClampMode));
				j.kv("vu1_clamp_mode", clampModeName(entry->vu1ClampMode));

				j.key("game_fixes"); j.startArray();
				for (GamefixId fix : entry->gameFixes)
					j.valStr(Pcsx2Config::GamefixOptions::GetGameFixName(fix));
				j.endArray();

				// GameDB-embedded compatibility patches for this exact CRC (distinct
				// from user-installed pnach patches/cheats below) - these are the
				// project's own known-safe fixes and often name/target specific
				// addresses directly in their pnach text.
				const std::string* rawPatch = entry->findPatch(crc);
				if (rawPatch)
					j.kv("gamedb_patch", *rawPatch);
			}
			j.endObject();
		}
		// ----- LIST GAME PATCHES/CHEATS -----
		else if (cmd == "list_game_patches")
		{
			bool alive = cpu->isAlive();
			if (!alive)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "No game loaded");
				j.endObject();
				return j.str();
			}

			bool includeCheats = getBool(params, "cheats", true);

			std::string serial;
			u32 crc = 0;
			runOnCpuBlocking([&]() {
				serial = VMManager::GetDiscSerial();
				crc = VMManager::GetDiscCRC();
			});

			u32 numUnlabelled = 0;
			// Note: these are community-authored pnach patches/cheats (name +
			// human description + author only - no raw addresses, unlike
			// get_gamedb_info's embedded patch text). Still valuable: the
			// description alone often hints at what a cheat does and where to
			// look (e.g. "Infinite Health", "Unlock All Levels").
			std::vector<Patch::PatchInfo> patches = Patch::GetPatchInfo(serial, crc, includeCheats, false, &numUnlabelled);

			j.startObject();
			j.kv("ok", true);
			j.kv("unlabelled_count", (int64_t)numUnlabelled);
			j.key("patches"); j.startArray();
			for (const auto& p : patches)
			{
				j.startObject();
				j.kv("name", p.name);
				if (!p.description.empty())
					j.kv("description", p.description);
				if (!p.author.empty())
					j.kv("author", p.author);
				j.endObject();
			}
			j.endArray();
			j.endObject();
		}
		// ============================================================
		// VU1 registers - bypasses DebugInterface entirely (VU1 isn't part of
		// its EECAT_* category contract, unlike VU0/GS which piggyback on
		// read_registers already). VU1 shares the exact same VURegs struct as
		// VU0 (extern VURegs vuRegs[2]; VU0/VU1 are just #defines for
		// vuRegs[0]/vuRegs[1]), so this reads vuRegs[1] directly and reuses the
		// same R5900::COP2_REG_FP/COP2_REG_CTL name tables VU0 already uses.
		// ============================================================
		// ----- READ VU1 REGISTERS -----
		else if (cmd == "read_vu1_registers")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			j.startObject();
			j.kv("ok", true);
			j.key("data"); j.startObject();

			runOnCpuBlocking([&]() {
				j.key("VU1f"); j.startObject();
				j.kv("size", 128);
				j.kv("count", 33);
				j.key("regs"); j.startArray();
				for (int r = 0; r < 32; r++)
				{
					j.startObject();
					j.kv("name", R5900::COP2_REG_FP[r]);
					j.key("value"); j.valHex128(vuRegs[1].VF[r].UQ);
					j.endObject();
				}
				j.startObject();
				j.kv("name", "ACC");
				j.key("value"); j.valHex128(vuRegs[1].ACC.UQ);
				j.endObject();
				j.endArray();
				j.endObject();

				j.key("VU1i"); j.startObject();
				j.kv("size", 32);
				j.kv("count", 32);
				j.key("regs"); j.startArray();
				for (int r = 0; r < 32; r++)
				{
					j.startObject();
					j.kv("name", R5900::COP2_REG_CTL[r]);
					j.key("value"); j.valHex128(u128::From32(vuRegs[1].VI[r].UL));
					j.endObject();
				}
				j.endArray();
				j.endObject();
			});

			j.endObject();
			j.endObject();
		}
		// ============================================================
		// VU0/VU1 microcode: raw micro-program memory read, a real
		// upper/lower dual-issue disassembler, and address breakpoints.
		// Unlike EE/IOP, a VU instruction is two independently-encoded
		// 32-bit ops ("upper"/"lower") issued together every 8 bytes.
		// PCSX2 already ships disVU0MicroUF/LF and disVU1MicroUF/LF
		// (DebugTools/DisVU0Micro.cpp / DisVU1Micro.cpp) - they just weren't
		// wired up to anything before this (no VU view exists anywhere in
		// the Qt debugger either). Memory reads go straight through
		// vuRegs[n].Micro rather than DebugInterface::Read8, since VU
		// micro-mem isn't part of either DebugInterface's normal memory view
		// the way EE/IOP RAM is.
		//
		// Breakpoints are the one piece that's a genuine capability gap, not
		// just missing plumbing: they're checked from VUBreakpoints.h, which
		// is itself only called from the VU0/VU1 *interpreters*
		// (VU0microInterp.cpp/VU1microInterp.cpp). PCSX2's default execution
		// path is the microVU JIT recompiler, which - unlike the EE/IOP
		// recompilers - has no per-instruction breakpoint bailout mechanism
		// at all. See VUBreakpoints.h's header comment for the full
		// explanation; set_vu_breakpoint's response surfaces a warning field
		// when the affected VU is currently running under its recompiler.
		// ============================================================
		// ----- READ VU MICRO PROGRAM MEMORY -----
		else if (cmd == "read_vu_micromem")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int vu = (int)getNum(params, "vu", 0);
			if (vu != 0 && vu != 1) vu = 0;
			const u32 progSize = (vu == 0) ? VU0_PROGSIZE : VU1_PROGSIZE;

			u32 addr = (u32)getNum(params, "address", 0);
			int len = (int)getNum(params, "length", 256);
			if (len < 0) len = 0;
			if (len > 65536) len = 65536;
			if (addr > progSize) addr = progSize;
			if (addr + (u32)len > progSize) len = (int)(progSize - addr);

			std::string hexStr;
			runOnCpuBlocking([&]() {
				const u8* mem = vuRegs[vu].Micro;
				hexStr.reserve((size_t)len * 2);
				char hb[4];
				for (int i = 0; i < len; i++)
				{
					snprintf(hb, sizeof(hb), "%02x", mem[addr + i]);
					hexStr += hb;
				}
			});

			j.startObject();
			j.kv("ok", true);
			j.kv("vu", vu);
			j.key("address"); j.valHex32(addr);
			j.kv("length", (int64_t)len);
			j.kv("prog_size", (int64_t)progSize);
			j.key("hex"); j.valStr(hexStr);
			j.endObject();
		}
		// ----- DISASSEMBLE VU MICROCODE -----
		else if (cmd == "disassemble_vu")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			int vu = (int)getNum(params, "vu", 0);
			if (vu != 0 && vu != 1) vu = 0;
			const u32 progSize = (vu == 0) ? VU0_PROGSIZE : VU1_PROGSIZE;

			u32 addr = (u32)getNum(params, "address", 0) & ~7u; // VU instrs are always 8-aligned
			int count = (int)getNum(params, "count", 20);
			if (count < 0) count = 0;
			if (count > 500) count = 500;

			j.startObject();
			j.kv("ok", true);
			j.kv("vu", vu);
			j.key("instructions"); j.startArray();

			runOnCpuBlocking([&]() {
				const u8* mem = vuRegs[vu].Micro;
				for (int i = 0; i < count; i++)
				{
					u32 pc = addr + (u32)i * 8;
					if (pc + 8 > progSize) break;

					// Instructions are fetched as two u32 words (lower at +0, upper
					// at +4) - confirmed against the interpreters themselves
					// (VU0microInterp.cpp/VU1microInterp.cpp both do
					// `ptr = (u32*)&Micro[TPC]; ptr[0]` = lower, `ptr[1]` = upper).
					// memcpy rather than a cast+deref: mem+pc is only guaranteed
					// byte-aligned here (a caller-supplied `address` need not be
					// naturally u32-aligned before the `& ~7u` above establishes
					// 8-byte alignment - which does happen to satisfy 4-byte
					// alignment too, but relying on that indirectly instead of
					// just being explicit is asking for a future edit to break it).
					u32 lower, upper;
					std::memcpy(&lower, mem + pc, 4);
					std::memcpy(&upper, mem + pc + 4, 4);

					// Flag bits live in the upper word - confirmed against the
					// interpreters' own `ptr[1] & 0x..` checks.
					bool iflag = (upper & 0x80000000) != 0; // lower word is a raw float immediate (LOI), not an opcode
					bool eflag = (upper & 0x40000000) != 0; // end of microprogram (2 more instructions still execute after this one)
					bool mflag = (upper & 0x20000000) != 0; // VU0 only
					bool dflag = (upper & 0x10000000) != 0; // debug/breakpoint interrupt
					bool tflag = (upper & 0x08000000) != 0; // trace interrupt

					// disVU0MicroUF/LF and disVU1MicroUF/LF each write into a
					// single static buffer *shared* between their own upper and
					// lower variants (see the `static char ostr[1024]` at the top
					// of DisVU0Micro.cpp / DisVU1Micro.cpp) - the lower call
					// overwrites what the upper call just returned, so upper's
					// result must be copied out to a std::string before lower is
					// called, not held as a dangling char*.
					std::string upperText = stripDisVuPrefix(
						(vu == 0) ? disVU0MicroUF(upper, pc) : disVU1MicroUF(upper, pc));

					std::string lowerText;
					if (iflag)
					{
						// I flag: the lower word isn't an opcode at all, it's a raw
						// f32 that gets latched into VI[REG_I] (confirmed in both
						// interpreters: `VU->VI[REG_I].UL = ptr[0]` right after the
						// I-flag branch) - so there's nothing to disassemble, show
						// the decoded float value directly instead.
						float fval;
						std::memcpy(&fval, &lower, 4);
						char buf[64];
						snprintf(buf, sizeof(buf), "LOI %.9g (0x%08x)", (double)fval, lower);
						lowerText = buf;
					}
					else
					{
						lowerText = stripDisVuPrefix(
							(vu == 0) ? disVU0MicroLF(lower, pc) : disVU1MicroLF(lower, pc));
					}

					j.startObject();
					j.key("address"); j.valHex32(pc);
					j.key("upper_opcode"); j.valHex32(upper);
					j.key("lower_opcode"); j.valHex32(lower);
					j.key("flags"); j.startObject();
					j.kv("i", iflag);
					j.kv("e", eflag);
					j.kv("m", mflag);
					j.kv("d", dflag);
					j.kv("t", tflag);
					j.endObject();
					j.kv("upper", upperText);
					j.kv("lower", lowerText);
					j.endObject();
					// Deliberately not auto-stopping on eflag: real hardware still
					// executes 2 more instructions after E is set (it's a delay-slot-
					// like mechanism, not an immediate halt), so a caller disassembling
					// past what looks like "the end" is very likely intentional, not a bug.
				}
			});

			j.endArray();
			j.endObject();
		}
		// ----- SET VU BREAKPOINT -----
		else if (cmd == "set_vu_breakpoint")
		{
			int vu = (int)getNum(params, "vu", 0);
			if (vu != 0 && vu != 1)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "vu must be 0 or 1");
				j.endObject();
				return j.str();
			}
			u32 addr = (u32)getNum(params, "address", 0) & ~7u;
			std::string desc = getStr(params, "description", "");

			VUBreakpoints::Add(vu, addr, desc);

			bool usesRecompiler = (vu == 0) ? EmuConfig.Cpu.Recompiler.EnableVU0 : EmuConfig.Cpu.Recompiler.EnableVU1;

			j.startObject();
			j.kv("ok", true);
			j.kv("vu", vu);
			j.key("address"); j.valHex32(addr);
			if (usesRecompiler)
			{
				j.kv("warning", (vu == 0)
					? "VU0 is currently running under the microVU recompiler - this breakpoint will not fire until you disable 'Enable VU0 Recompiler' in System > Emulation (interpreted VU is much slower, so only do this for the VU you're actively investigating)"
					: "VU1 is currently running under the microVU recompiler - this breakpoint will not fire until you disable 'Enable VU1 Recompiler' in System > Emulation (interpreted VU is much slower, so only do this for the VU you're actively investigating)");
			}
			j.endObject();
		}
		// ----- REMOVE VU BREAKPOINT -----
		else if (cmd == "remove_vu_breakpoint")
		{
			int vu = (int)getNum(params, "vu", 0);
			if (vu != 0 && vu != 1) vu = 0;
			u32 addr = (u32)getNum(params, "address", 0) & ~7u;
			VUBreakpoints::Remove(vu, addr);
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ----- LIST VU BREAKPOINTS -----
		else if (cmd == "list_vu_breakpoints")
		{
			int vu = (int)getNum(params, "vu", -1); // -1 = both
			j.startObject();
			j.kv("ok", true);
			j.key("breakpoints"); j.startArray();
			auto emit = [&](int v) {
				for (const auto& bp : VUBreakpoints::List(v))
				{
					j.startObject();
					j.kv("vu", v);
					j.key("address"); j.valHex32(bp.first);
					if (!bp.second.empty())
						j.kv("description", bp.second);
					j.endObject();
				}
			};
			if (vu == 0 || vu == 1) emit(vu);
			else { emit(0); emit(1); }
			j.endArray();
			j.endObject();
		}
		// ----- CLEAR VU BREAKPOINTS -----
		else if (cmd == "clear_vu_breakpoints")
		{
			int vu = (int)getNum(params, "vu", -1); // -1 = both
			if (vu == 0 || vu == 1) VUBreakpoints::Clear(vu);
			else { VUBreakpoints::Clear(0); VUBreakpoints::Clear(1); }
			j.startObject();
			j.kv("ok", true);
			j.endObject();
		}
		// ============================================================
		// GS drawing-context/environment state. GS has no programmable
		// microcode to disassemble (unlike VU) - this is the analogous
		// feature for GS: a full state dump instead. Reads are marshaled
		// through runOnGsBlocking (the GS thread, not the CPU thread - GS
		// runs on its own dedicated "MTGS" thread, confirmed via
		// MTGS::RunOnGSThread usage throughout GS.cpp).
		// ============================================================
		// ----- GET GS CONTEXT -----
		else if (cmd == "get_gs_context")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			j.startObject();
			j.kv("ok", true);

			runOnGsBlocking([&]() {
				if (!g_gs_renderer)
				{
					j.kv("gs_ready", false);
					return;
				}
				j.kv("gs_ready", true);

				const GSDrawingEnvironment& env = g_gs_renderer->m_env;
				int activeCtx = (g_gs_renderer->m_context == &env.CTXT[1]) ? 1 : 0;
				j.kv("active_context", (int64_t)activeCtx);

				// ---- Environment-level state ----
				j.key("environment"); j.startObject();

				j.key("prim"); j.startObject();
				j.kv("raw", hex64(env.PRIM.U64));
				j.kv("prim", (int64_t)env.PRIM.PRIM);
				j.kv("iip", env.PRIM.IIP != 0);
				j.kv("tme", env.PRIM.TME != 0);
				j.kv("fge", env.PRIM.FGE != 0);
				j.kv("abe", env.PRIM.ABE != 0);
				j.kv("aa1", env.PRIM.AA1 != 0);
				j.kv("fst", env.PRIM.FST != 0);
				j.kv("ctxt", (int64_t)env.PRIM.CTXT);
				j.kv("fix", env.PRIM.FIX != 0);
				j.endObject();

				j.key("prmode"); j.startObject();
				j.kv("raw", hex64(env.PRMODE.U64));
				j.endObject();

				j.key("colclamp"); j.startObject();
				j.kv("raw", hex64(env.COLCLAMP.U64));
				j.kv("clamp", env.COLCLAMP.CLAMP != 0);
				j.endObject();

				j.key("dthe"); j.startObject();
				j.kv("raw", hex64(env.DTHE.U64));
				j.kv("dthe", env.DTHE.DTHE != 0);
				j.endObject();

				j.key("scanmsk"); j.startObject();
				j.kv("raw", hex64(env.SCANMSK.U64));
				j.kv("msk", (int64_t)env.SCANMSK.MSK);
				j.endObject();

				j.key("fogcol"); j.startObject();
				j.kv("raw", hex64(env.FOGCOL.U64));
				j.kv("r", (int64_t)env.FOGCOL.FCR);
				j.kv("g", (int64_t)env.FOGCOL.FCG);
				j.kv("b", (int64_t)env.FOGCOL.FCB);
				j.endObject();

				j.endObject(); // environment

				// ---- Per-context drawing state (both contexts; active one flagged) ----
				j.key("contexts"); j.startArray();
				for (int c = 0; c < 2; c++)
				{
					const GSDrawingContext& ctx = env.CTXT[c];
					j.startObject();
					j.kv("index", (int64_t)c);
					j.kv("active", c == activeCtx);

					j.key("xyoffset"); j.startObject();
					j.kv("raw", hex64(ctx.XYOFFSET.U64));
					j.kv("ofx", (int64_t)ctx.XYOFFSET.OFX);
					j.kv("ofy", (int64_t)ctx.XYOFFSET.OFY);
					j.endObject();

					j.key("tex0"); j.startObject();
					j.kv("raw", hex64(ctx.TEX0.U64));
					j.kv("tbp0", (int64_t)ctx.TEX0.TBP0);
					j.kv("tbw", (int64_t)ctx.TEX0.TBW);
					j.kv("psm", (int64_t)ctx.TEX0.PSM);
					j.kv("tw", (int64_t)ctx.TEX0.TW);
					j.kv("th", (int64_t)ctx.TEX0.TH);
					j.kv("tcc", ctx.TEX0.TCC != 0);
					j.kv("tfx", (int64_t)ctx.TEX0.TFX);
					j.kv("cbp", (int64_t)ctx.TEX0.CBP);
					j.kv("cpsm", (int64_t)ctx.TEX0.CPSM);
					j.kv("csm", (int64_t)ctx.TEX0.CSM);
					j.kv("csa", (int64_t)ctx.TEX0.CSA);
					j.kv("cld", (int64_t)ctx.TEX0.CLD);
					j.endObject();

					j.key("tex1"); j.startObject();
					j.kv("raw", hex64(ctx.TEX1.U64));
					j.endObject();

					j.key("clamp"); j.startObject();
					j.kv("raw", hex64(ctx.CLAMP.U64));
					j.kv("wms", (int64_t)ctx.CLAMP.WMS);
					j.kv("wmt", (int64_t)ctx.CLAMP.WMT);
					j.kv("minu", (int64_t)ctx.CLAMP.MINU);
					j.kv("maxu", (int64_t)ctx.CLAMP.MAXU);
					j.kv("minv", (int64_t)ctx.CLAMP.MINV);
					j.kv("maxv", (int64_t)ctx.CLAMP.MAXV);
					j.endObject();

					j.key("miptbp1"); j.startObject();
					j.kv("raw", hex64(ctx.MIPTBP1.U64));
					j.endObject();

					j.key("miptbp2"); j.startObject();
					j.kv("raw", hex64(ctx.MIPTBP2.U64));
					j.endObject();

					j.key("scissor"); j.startObject();
					j.kv("raw", hex64(ctx.SCISSOR.U64));
					j.kv("scax0", (int64_t)ctx.SCISSOR.SCAX0);
					j.kv("scax1", (int64_t)ctx.SCISSOR.SCAX1);
					j.kv("scay0", (int64_t)ctx.SCISSOR.SCAY0);
					j.kv("scay1", (int64_t)ctx.SCISSOR.SCAY1);
					j.endObject();

					j.key("alpha"); j.startObject();
					j.kv("raw", hex64(ctx.ALPHA.U64));
					j.kv("a", (int64_t)ctx.ALPHA.A);
					j.kv("b", (int64_t)ctx.ALPHA.B);
					j.kv("c", (int64_t)ctx.ALPHA.C);
					j.kv("d", (int64_t)ctx.ALPHA.D);
					j.kv("fix", (int64_t)ctx.ALPHA.FIX);
					j.endObject();

					j.key("test"); j.startObject();
					j.kv("raw", hex64(ctx.TEST.U64));
					j.kv("ate", ctx.TEST.ATE != 0);
					j.kv("atst", (int64_t)ctx.TEST.ATST);
					j.kv("aref", (int64_t)ctx.TEST.AREF);
					j.kv("afail", (int64_t)ctx.TEST.AFAIL);
					j.kv("date", ctx.TEST.DATE != 0);
					j.kv("datm", ctx.TEST.DATM != 0);
					j.kv("zte", ctx.TEST.ZTE != 0);
					j.kv("ztst", (int64_t)ctx.TEST.ZTST);
					j.endObject();

					j.key("fba"); j.startObject();
					j.kv("raw", hex64(ctx.FBA.U64));
					j.kv("fba", ctx.FBA.FBA != 0);
					j.endObject();

					j.key("frame"); j.startObject();
					j.kv("raw", hex64(ctx.FRAME.U64));
					j.kv("fbp", (int64_t)ctx.FRAME.FBP);
					j.kv("fbw", (int64_t)ctx.FRAME.FBW);
					j.kv("psm", (int64_t)ctx.FRAME.PSM);
					char fbmskBuf[12];
					snprintf(fbmskBuf, sizeof(fbmskBuf), "0x%08x", ctx.FRAME.FBMSK);
					j.kv("fbmsk", fbmskBuf);
					j.endObject();

					j.key("zbuf"); j.startObject();
					j.kv("raw", hex64(ctx.ZBUF.U64));
					j.kv("zbp", (int64_t)ctx.ZBUF.ZBP);
					j.kv("psm", (int64_t)ctx.ZBUF.PSM);
					j.kv("zmsk", ctx.ZBUF.ZMSK != 0);
					j.endObject();

					j.endObject(); // this context
				}
				j.endArray(); // contexts
			});

			j.endObject();
		}
		// ============================================================
		// Screenshot capture. GSRenderer::SaveSnapshotToMemory() already does
		// exactly what's needed here (used internally for PCSX2's own
		// File > Save Screenshot and GS dump features) - it downloads the
		// current presented frame from the GPU into a plain RGBA8 pixel
		// buffer. That download touches g_gs_device, so it's marshaled
		// through runOnGsBlocking same as get_gs_context above. Encoding the
		// pixels to PNG/JPEG/WebP afterward is pure CPU work (libpng/
		// libjpeg/libwebp via RGBA8Image::SaveToBuffer(), entirely in
		// memory - no temp files), so it happens back on this client thread
		// rather than occupying the GS thread for longer than necessary.
		// ----- GET SCREENSHOT -----
		else if (cmd == "get_screenshot")
		{
			if (!requireAlive(cpu, j))
				return j.str();

			std::string format = getStr(params, "format", "png");
			if (format != "png" && format != "jpg" && format != "jpeg" && format != "webp")
				format = "png";

			int quality = (int)getNum(params, "quality", RGBA8Image::DEFAULT_SAVE_QUALITY);
			if (quality < 1) quality = 1;
			if (quality > 100) quality = 100;

			// 0 = use GS's internal render resolution (native, unscaled) rather
			// than fitting to some particular output window size - there is no
			// "window" here, just an off-screen GPU readback, so native is the
			// only size that means anything by default.
			u32 reqWidth = (u32)getNum(params, "width", 0);
			u32 reqHeight = (u32)getNum(params, "height", 0);
			bool applyAspect = getBool(params, "apply_aspect", true);
			bool cropBorders = getBool(params, "crop_borders", true);

			bool gsReady = false;
			bool captured = false;
			u32 width = 0, height = 0;
			std::vector<u32> pixels;

			runOnGsBlocking([&]() {
				if (!g_gs_renderer)
				{
					gsReady = false;
					return;
				}
				gsReady = true;
				captured = g_gs_renderer->SaveSnapshotToMemory(reqWidth, reqHeight, applyAspect, cropBorders,
					&width, &height, &pixels);
			});

			if (!gsReady)
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "GS is not ready yet (no frame rendered since boot)");
				j.endObject();
				return j.str();
			}
			if (!captured || pixels.empty())
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "Failed to capture frame - no output texture available yet (e.g. still on a black/loading screen)");
				j.endObject();
				return j.str();
			}

			RGBA8Image image;
			image.SetPixels(width, height, std::move(pixels));

			// SaveToBuffer() picks its codec from the extension on this string,
			// not from an actual file - nothing here ever touches disk.
			const std::string fakeFilename = "screenshot." + format;
			std::optional<std::vector<u8>> encoded = image.SaveToBuffer(fakeFilename.c_str(), (u8)quality);
			if (!encoded.has_value())
			{
				j.startObject();
				j.kv("ok", false);
				j.kv("error", "Image encode failed");
				j.endObject();
				return j.str();
			}

			j.startObject();
			j.kv("ok", true);
			j.kv("width", (int64_t)width);
			j.kv("height", (int64_t)height);
			j.kv("format", format);
			j.kv("encoded_bytes", (int64_t)encoded->size());
			j.kv("data_base64", base64Encode(encoded->data(), encoded->size()));
			j.endObject();
		}
		// ----- UNKNOWN COMMAND -----
		else
		{
			j.startObject();
			j.kv("ok", false);
			j.kv("error", "Unknown command: " + cmd);
			j.key("available_commands"); j.startArray();
			const char* cmds[] = {
				"status", "read_registers", "write_register", "set_pc",
				"read_memory", "write_memory", "read_memory_multiple", "read_string",
				"disassemble", "evaluate",
				"set_breakpoint", "remove_breakpoint", "list_breakpoints",
				"set_memcheck", "remove_memcheck", "list_memchecks",
				"pause", "resume", "resume_and_wait", "step", "step_over",
				"get_threads", "get_modules", "get_backtrace",
				"is_valid_address", "clear_breakpoints",
				"resolve_address", "find_symbol", "get_function_info", "list_functions",
				"decode_instruction", "assemble", "scan_functions",
				"search_memory_start", "search_memory_next", "search_memory_results", "search_memory_reset",
				"get_game_info", "reset_vm", "frame_advance", "save_state_slot", "load_state_slot", "change_disc",
				"get_gamedb_info", "list_game_patches",
				"read_vu1_registers", "get_gs_context", "get_screenshot",
				"read_vu_micromem", "disassemble_vu", "set_vu_breakpoint", "remove_vu_breakpoint", "list_vu_breakpoints", "clear_vu_breakpoints"
			};
			for (const char* c : cmds) j.valStr(c);
			j.endArray();
			j.endObject();
		}

		return j.str();
	}

	// ============================================================
	// TCP Server
	// ============================================================
	static std::atomic<bool> s_running{false};
	static std::thread s_serverThread;
	static socket_t s_listenSocket = SOCKET_INVALID;

	// PCSX2-MCP fix: Stop() used to only close the listen socket, leaving any
	// already-accepted client sockets open. Each clientHandler thread blocks in a
	// plain recv() with no timeout, so an idle-but-connected MCP client (normal —
	// pcsx2mcp holds the connection open for the whole session) left that thread
	// parked forever, which was blocking clean process exit until Ctrl+C/kill.
	// Track every accepted client socket here so Stop() can shut them all down.
	static std::mutex s_clientsMutex;
	static std::set<socket_t> s_clientSockets;

	// A single send() call is not guaranteed to transmit the whole buffer -
	// the kernel socket send buffer can fill up and hand back a short write,
	// especially for large replies (e.g. get_screenshot's base64 payload can
	// easily be several hundred KB to a couple MB, well past typical socket
	// buffer sizes). Every reply up to now has been small enough in practice
	// to fit in one send(), which is exactly why this went unnoticed - but it
	// silently truncates the JSON line for any reply large enough to trigger
	// it, breaking the client's JSON.parse(). Same fix already applied to
	// PINE.cpp's ClientLoop() for the same reason; ported here.
	static bool sendAll(socket_t sock, const char* data, size_t len)
	{
		size_t sent = 0;
		while (sent < len)
		{
			const int result = send(sock, data + sent, (int)(len - sent), 0);
			if (result <= 0)
				return false;
			sent += (size_t)result;
		}
		return true;
	}

	static void clientHandler(socket_t clientSock)
	{
		{
			std::lock_guard<std::mutex> lock(s_clientsMutex);
			s_clientSockets.insert(clientSock);
		}

		std::string buffer;
		char recvBuf[4096];

		while (s_running.load())
		{
			int bytes = recv(clientSock, recvBuf, sizeof(recvBuf) - 1, 0);
			if (bytes <= 0) break;

			recvBuf[bytes] = '\0';
			buffer += recvBuf;

			// Process complete lines
			size_t newlinePos;
			while ((newlinePos = buffer.find('\n')) != std::string::npos)
			{
				std::string line = buffer.substr(0, newlinePos);
				buffer = buffer.substr(newlinePos + 1);

				// Trim
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
					line.pop_back();

				if (line.empty()) continue;

				std::string response = handleCommand(line);
				response += "\n";

				if (!sendAll(clientSock, response.c_str(), response.size()))
					goto clientDisconnected;
			}
		}

	clientDisconnected:
		{
			std::lock_guard<std::mutex> lock(s_clientsMutex);
			s_clientSockets.erase(clientSock);
		}
		CLOSE_SOCKET(clientSock);
	}

	static void serverLoop(int port)
	{
#ifdef _WIN32
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		s_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s_listenSocket == SOCKET_INVALID)
		{
			fprintf(stderr, "[DebugServer] Failed to create socket\n");
			return;
		}

		int opt = 1;
		setsockopt(s_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

		struct sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
		addr.sin_port = htons((u_short)port);

		if (bind(s_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) != 0)
		{
			fprintf(stderr, "[DebugServer] Failed to bind on port %d\n", port);
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
			return;
		}

		if (listen(s_listenSocket, 2) != 0)
		{
			fprintf(stderr, "[DebugServer] Failed to listen\n");
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
			return;
		}

		fprintf(stderr, "[DebugServer] Listening on 127.0.0.1:%d\n", port);

		while (s_running.load())
		{
			// Use select with timeout to allow clean shutdown
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(s_listenSocket, &readSet);

			struct timeval tv;
			tv.tv_sec = 1;
			tv.tv_usec = 0;

			int selectResult = select((int)s_listenSocket + 1, &readSet, nullptr, nullptr, &tv);
			if (selectResult <= 0) continue;

			socket_t clientSock = accept(s_listenSocket, nullptr, nullptr);
			if (clientSock == SOCKET_INVALID) continue;

			fprintf(stderr, "[DebugServer] Client connected\n");

			// Handle client in a new thread
			std::thread(clientHandler, clientSock).detach();
		}

		CLOSE_SOCKET(s_listenSocket);
		s_listenSocket = SOCKET_INVALID;

#ifdef _WIN32
		WSACleanup();
#endif
	}

	void Start(int port)
	{
		if (s_running.load()) return;
		s_running.store(true);
		s_serverThread = std::thread(serverLoop, port);
		s_serverThread.detach();
	}

	void Stop()
	{
		s_running.store(false);
		if (s_listenSocket != SOCKET_INVALID)
		{
			CLOSE_SOCKET(s_listenSocket);
			s_listenSocket = SOCKET_INVALID;
		}

		// PCSX2-MCP fix: shutdown() every still-connected client socket so any
		// clientHandler thread parked in a blocking recv() (e.g. an idle-but-open
		// pcsx2mcp session) wakes up with an EOF/error, exits its loop, and the
		// detached thread terminates on its own instead of leaking past process
		// "shutdown" and requiring Ctrl+C/kill to actually exit.
		{
			std::lock_guard<std::mutex> lock(s_clientsMutex);
			for (socket_t sock : s_clientSockets)
			{
#ifdef _WIN32
				shutdown(sock, SD_BOTH);
#else
				shutdown(sock, SHUT_RDWR);
#endif
			}
		}
	}

	bool IsRunning()
	{
		return s_running.load();
	}

	void OnBreakpointHit()
	{
		// Future: notify connected clients of breakpoint events
	}

} // namespace DebugServer
