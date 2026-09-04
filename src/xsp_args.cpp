#include "xsp_args.h"
#include "xsp_server.h"

#include "xsp_blend.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace xsp {
namespace args {

// The machine the arguments are read against: an address can be a label, and a
// label only means something to the symbol table that is loaded.
static Machine& mach() { return server::machine(); }

// Every address argument goes through argAddr(), so they all take the same things.
const char* const kAddrArg = "address: a number, \"$hex\"/\"0x\"/\"#hex\", or the name of a loaded label";

// The ceiling on anything measured in frames. It is not a performance limit -
// it is what keeps a mistyped argument from turning into a call that never
// comes back. A million frames is around five and a half hours of emulated
// time, and about half an hour of real time on the machine this was measured
// on, so it is well past any honest use and well short of forever.
const int kMaxFrames = 1000000;

const long long kMaxInstructions = 2000000000LL;

// A number: a JSON number, or a string in any of the usual notations. Present but
// unreadable is an error, never a silent fallback to the default - a typo that
// quietly becomes zero is worse than a refusal.
bool argNum(const json& a, const char* key, int& out) {
	if (!a.is_object() || !a.contains(key) || a[key].is_null()) return false;
	const json& v = a[key];
	if (v.is_number_integer()) { out = v.get<int>(); return true; }
	if (v.is_number_float()) { out = (int)v.get<double>(); return true; }
	if (v.is_string() && xsp::parseNumber(v.get<std::string>(), out)) return true;
	throw std::runtime_error(std::string("bad ") + key + ": " + v.dump() + " is not a number");
}

int argNumOr(const json& a, const char* key, int def) {
	int v = def;
	argNum(a, key, v);
	return v;
}

// The same, but with an interval the value has to be inside. Out of range is a
// refusal rather than a clamp, for the reason above: an argument the caller
// cannot have meant is a mistake worth reporting, and quietly moving it answers
// a question nobody asked. The default is trusted - it is ours.
int argNumRange(const json& a, const char* key, int def, int lo, int hi) {
	int v = def;
	if (!argNum(a, key, v)) return def;
	if (v < lo || v > hi)
		throw std::runtime_error(std::string("bad ") + key + ": " + std::to_string(v) +
					 " is outside " + std::to_string(lo) + ".." + std::to_string(hi));
	return v;
}

// An address: a number in any notation, or the name of a label from the loaded
// symbol table. Used for every argument where an address is meaningful, so
// {"address": "main_loop"} works wherever {"address": "$600E"} does.
bool argAddr(const json& a, const char* key, int& out) {
	if (!a.is_object() || !a.contains(key) || a[key].is_null()) return false;
	const json& v = a[key];
	if (v.is_number_integer()) { out = v.get<int>(); return true; }
	if (v.is_number_float()) { out = (int)v.get<double>(); return true; }
	if (!v.is_string())
		throw std::runtime_error(std::string("bad ") + key + ": " + v.dump() +
					 " is neither a number nor a label name");
	std::string s = xsp::trim(v.get<std::string>());
	int n = 0;
	if (xsp::parseNumber(s, n)) { out = n; return true; }
	if (mach().labels().find(s, n)) { out = n; return true; }
	throw std::runtime_error("no such label '" + s + "' (" + key + " takes a number, $hex or a "
				 "label name; " + (mach().labels().size()
					? std::to_string(mach().labels().size()) + " labels loaded from " +
					  mach().labels().source()
					: std::string("no symbol table loaded - see load_labels")) + ")");
}

// An address that is going to index something 64K long, so it has to be one.
// Everything that walks a range of memory takes its ends through here: the
// Z80 cannot reach anything else, and a number that is not an address has
// nowhere sensible to be clamped to.
int argAddrRange(const json& a, const char* key, int def) {
	int v = def;
	if (!argAddr(a, key, v)) return def;
	if (v < 0 || v > 0xffff)
		throw std::runtime_error(std::string("bad ") + key + ": " + std::to_string(v) +
					 " is outside the address space ($0000..$FFFF)");
	return v;
}

int argAddrRequired(const json& a, const char* key) {
	int v = 0;
	if (!argAddr(a, key, v))
		throw std::runtime_error(std::string(key) + " required");
	if (v < 0 || v > 0xffff)
		throw std::runtime_error(std::string("bad ") + key + ": " + std::to_string(v) +
					 " is outside the address space ($0000..$FFFF)");
	return v;
}

// As argAddr(), but also reports which page the address belongs to when the
// argument carries one: a label loaded from a "BB:OOOO name" line, or the same
// "bank:offset" written out by hand (both parts hex, as in the symbol file).
// bank stays -1 for a plain address, which means "wherever it is mapped now".
bool argAddrBank(const json& a, const char* key, int& out, int& bank) {
	bank = -1;
	if (a.is_object() && a.contains(key) && a[key].is_string()) {
		std::string s = xsp::trim(a[key].get<std::string>());
		size_t colon = s.find(':');
		if (colon != std::string::npos && colon > 0) {
			int b = 0, off = 0;
			if (xsp::parseNumber("$" + s.substr(0, colon), b) &&
			    xsp::parseNumber("$" + s.substr(colon + 1), off)) {
				bank = b;
				out = xsp::Labels::mapBankOffset(b, off);
				return true;
			}
		}
		if (const auto* e = mach().labels().entry(s)) {
			out = e->address;
			bank = e->bank;
			return true;
		}
	}
	return argAddr(a, key, out);
}

std::string argStr(const json& a, const char* key, const std::string& def) {
	if (!a.is_object() || !a.contains(key) || !a[key].is_string()) return def;
	return a[key].get<std::string>();
}

bool argBool(const json& a, const char* key, bool def) {
	if (!a.is_object() || !a.contains(key) || !a[key].is_boolean()) return def;
	return a[key].get<bool>();
}

bool argDouble(const json& a, const char* key, double& out) {
	if (!a.is_object() || !a.contains(key) || a[key].is_null()) return false;
	const json& v = a[key];
	if (v.is_number()) { out = v.get<double>(); return true; }
	if (v.is_string()) {
		// strtod with the end checked, not stod: stod stops at the first thing
		// it cannot read and reports success, so "2.2rubbish" came through as
		// 2.2 while the settings parser refused the same string. One of the two
		// was wrong about the project's own rule, and it was this one.
		const std::string s = xsp::trim(v.get<std::string>());
		if (!s.empty()) {
			char* end = nullptr;
			const double d = strtod(s.c_str(), &end);
			if (end && *end == '\0') { out = d; return true; }
		}
	}
	throw std::runtime_error(std::string("bad ") + key + ": " + v.dump() + " is not a number");
}

// How many frames a picture is made of. Shared by every tool that produces one,
// so `blend` means the same thing everywhere. Absent = whatever video_config
// last set, true = two frames (a gigascreen), a number = that many.
int argBlend(const json& a) {
	if (!a.is_object() || !a.contains("blend") || a["blend"].is_null()) return -1;
	if (a["blend"].is_boolean()) return a["blend"].get<bool>() ? 2 : 1;
	return argNumRange(a, "blend", -1, 0, xsp::blend::kMaxHistory);
}

// What the blend did, reported next to the picture it made. `distinct` is the
// part worth reading: an effect that holds each picture for two interrupts
// blends two identical frames, and the result looks unmixed for a reason that
// has nothing to do with the blend being broken.
json blendJson(const xsp::video::BlendInfo& bi) {
	json j{{"frames", bi.frames}, {"frames_used", bi.used},
	       {"distinct_frames", bi.distinct}, {"gamma", bi.gamma},
	       {"pattern", bi.pattern}};
	if (bi.used < bi.frames)
		j["note"] = "only " + std::to_string(bi.used) + " frame(s) in the history - run more "
			    "frames, or raise frame_history in video_config";
	else if (bi.distinct < bi.used)
		j["note"] = "the blended frames are not all different: this effect repeats a picture "
			    "for more than one interrupt, so blending " + std::to_string(bi.used) +
			    " frames mixes duplicates. Try a larger blend.";
	// Different frames are not on their own a reason to blend. Flicker means two
	// pictures alternating that are meant to be seen as one; an effect that is
	// simply animating also has different frames, and averaging those is motion
	// blur. Saying "they differ" would cover both and warn about neither.
	else if (bi.on && strcmp(bi.pattern, "animation") == 0)
		j["note"] = "these frames are animating rather than alternating: each one differs "
			    "from both of the last two, so blending them is motion blur rather than "
			    "the flicker being undone. Blend is for a picture that alternates.";
	return j;
}

// One byte written as bare hex, the way "3E 05 C9" spells it. strtol used to do
// this and took "GG" for zero and "1234" for a byte, so a typo turned into a
// pattern that searches for something else and finds it - or writes it.
unsigned char parseHexByte(const std::string& tok, const char* key) {
	auto bad = [&]() {
		return std::runtime_error(std::string("bad ") + key + ": '" + tok +
					  "' is not a hex byte (00..FF)");
	};
	if (tok.empty() || tok.size() > 2) throw bad();
	int v = 0;
	for (char ch : tok) {
		if (!isxdigit((unsigned char)ch)) throw bad();
		v = v * 16 + (isdigit((unsigned char)ch) ? ch - '0'
						        : tolower((unsigned char)ch) - 'a' + 10);
	}
	return (unsigned char)v;
}

std::string hex16(int v) {
	char buf[16];
	snprintf(buf, sizeof(buf), "$%04X", v & 0xffff);
	return buf;
}

SyncSpec argSync(const json& a, const char* key) {
	SyncSpec s;
	if (!a.is_object() || !a.contains(key) || a[key].is_null()) return s;
	if (a[key].is_string()) {
		std::string v = xsp::trim(a[key].get<std::string>());
		if (v == "halt") return s;
		if (v == "frame" || v == "interrupt") {
			s.byInterrupt = true;
			s.text = "frame";
			return s;
		}
	}
	int adr = 0;
	if (!argAddr(a, key, adr)) return s;
	s.pc = adr;
	s.text = hex16(adr);
	return s;
}

// "Wind past the precalculation first": run until PC reaches an address or a
// label before doing anything else. Every effect has a different init, and
// guessing it in frames is how recordings end up half black.
json skipUntil(const json& a, const char* key) {
	int adr = 0;
	if (!argAddr(a, key, adr)) return json();
	const long long budget = argNumRange(a, "skip_max_instructions", 200000000,
					    1, (int)kMaxInstructions);
	xsp::RunResult r = mach().run(budget, adr, -1);
	if (r.reason != "pc")
		throw std::runtime_error("never reached " + hex16(adr) + " in " +
					 std::to_string(budget) + " instructions (stopped: " +
					 r.reason + " at " + hex16(r.pc) + ")");
	const int nsPerT = mach().comp()->nsPerTick;
	return json{{"address", adr}, {"address_hex", hex16(adr)},
		    {"instructions", r.instructions},
		    {"t_states", nsPerT > 0 ? r.ns / nsPerT : 0},
		    {"frames", r.frames}};
}

json runToJson(const xsp::RunResult& r) {
	// T-states are the currency on a Z80, and they cannot be recovered from ns
	// afterwards: the core's nanosecond is a truncated integer (284 for 285.714),
	// so only the division it did itself gives the count back exactly.
	const int nsPerT = mach().comp() ? mach().comp()->nsPerTick : 0;
	json j = {
		{"reason", r.reason},
		{"pc", r.pc},
		{"pc_hex", hex16(r.pc)},
		{"instructions", r.instructions},
		{"frames", r.frames},
		{"ns", r.ns},
		{"t_states", nsPerT > 0 ? r.ns / nsPerT : 0}
	};
	if (r.reason == "breakpoint") {
		j["break_type"] = r.brk_type;
		j["break_address"] = r.brk_addr;
	}
	// Where the beam stood when this stopped. Raster code is debugged by asking
	// exactly this after every stop, and carrying it here saves the second call.
	if (r.beam_line >= 0) {
		j["beam"] = {
			{"line", r.beam_line}, {"dot", r.beam_dot},
			{"t_states_frame", r.beam_t}, {"frame_counter", r.beam_frame}
		};
	}
	return j;
}

} // namespace args
} // namespace xsp
