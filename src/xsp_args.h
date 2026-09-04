// Reading the arguments of a tool call.
//
// Every tool takes its arguments out of the same JSON object, and the rules for
// what a number, an address or a blend depth may look like are the same for all
// of them: that is the whole point of this file existing. When these lived
// inside the server's one translation unit they could not be tested and could
// not be reused, and a tool that read an argument its own way was
// impossible to spot.
//
// The shared rule underneath all of them: a value that is present but
// unreadable is refused, never quietly replaced by the default. A typo that
// becomes a zero is worse than an error message.
#pragma once

#include <string>

#include "json.hpp"

#include "xsp_machine.h"
#include "xsp_video.h"

namespace xsp {

using json = nlohmann::json;

namespace args {

// What every address argument accepts, quoted into the tools' own schemas so
// that the description and the parser cannot drift apart.
extern const char* const kAddrArg;

// The ceiling on anything measured in frames.
extern const int kMaxFrames;

// The ceiling on an instruction budget, and the reason every budget argument
// has to have one: execLoop treats a negative maximum as no maximum at all, so
// max_instructions: -1 is not a small budget, it is a call that never returns.
extern const long long kMaxInstructions;

// Where one "frame" ends. A hardware frame is an interrupt, but an effect frame
// is usually the code between two HALTs, and when a render overruns it spans
// several interrupts - so that is the default.
struct SyncSpec {
	bool byInterrupt = false;
	int pc = -1;			// -1: the CPU entering HALT
	std::string text = "halt";
};

bool argNum(const json& a, const char* key, int& out);
int argNumOr(const json& a, const char* key, int def);
int argNumRange(const json& a, const char* key, int def, int lo, int hi);
bool argAddr(const json& a, const char* key, int& out);
int argAddrRange(const json& a, const char* key, int def);
// Required and inside the address space, which is what most callers of argAddr
// actually mean. Reading past $FFFF used to answer with the byte at the wrapped
// address while reporting the number that was asked for, so the reply contained
// both the question and a different question's answer.
int argAddrRequired(const json& a, const char* key);
bool argAddrBank(const json& a, const char* key, int& out, int& bank);
std::string argStr(const json& a, const char* key, const std::string& def = "");
bool argBool(const json& a, const char* key, bool def);
bool argDouble(const json& a, const char* key, double& out);
int argBlend(const json& a);
json blendJson(const xsp::video::BlendInfo& bi);
unsigned char parseHexByte(const std::string& tok, const char* key);
std::string hex16(int v);
SyncSpec argSync(const json& a, const char* key = "sync");
json skipUntil(const json& a, const char* key = "skip_until");
json runToJson(const xsp::RunResult& r);

} // namespace args
} // namespace xsp
