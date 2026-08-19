// MCP server for the Xpeccy ZX Spectrum core: JSON-RPC 2.0 over stdio,
// one message per line, logs on stderr.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

#include "xsp_keyboard.h"
#include "xsp_platform.h"
#include "xsp_record.h"
#include "xsp_machine.h"
#include "xsp_video.h"

extern "C" {
#include "filetypes/filetypes.h"
}

using json = nlohmann::json;
using xsp::Machine;

// All three come from the VERSIONS file at the top of the repository by way of
// cmake/version.cmake, so there is one place to change a version and no way for
// the build and build.py to end up claiming different things. The fallbacks are
// only for a compiler invoked by hand, outside the build system.
static const char* kServerName = "xspeccy";
#ifdef XSP_VERSION
static const char* kServerVersion = XSP_VERSION;
#else
static const char* kServerVersion = "0.0-adhoc";
#endif
// The Xpeccy the binary was actually built against, and the one it was meant to
// be. They differ when a tree was passed in with -DXPECCY_SRC, and a bug report
// that says which is worth a great deal more than one that does not.
#ifdef XSP_XPECCY_VERSION
static const char* kXpeccyVersion = XSP_XPECCY_VERSION;
#else
static const char* kXpeccyVersion = "unknown";
#endif
#ifdef XSP_XPECCY_REQUIRED
static const char* kXpeccyPinned = XSP_XPECCY_REQUIRED;
#else
static const char* kXpeccyPinned = "unknown";
#endif

// Sent with the initialize reply. A tool description can say what one tool does;
// it cannot say which of fifty-one to reach for, in what order, or which pairs
// of them answer questions that sound identical and are not. That is what this
// is for, and the protocol has the field precisely so a server can say it once
// instead of repeating it in every description.
//
// Kept short on purpose: it is prepended to the model's context for the whole
// session, so it earns its place by covering the choices that are actually
// costly to get wrong rather than by being complete.
static const char* kInstructions =
	"A ZX Spectrum (and clones) emulator driven as a tool. There is no window and no "
	"real-time throttle: it runs as fast as the host allows, and the same program produces "
	"the same output every time, so a measurement repeats exactly.\n"
	"\n"
	"Starting on a program:\n"
	"1. machine_config picks the model (Pentagon, ZX48K, Scorpion...). Changing it resets.\n"
	"2. load_file takes .sna/.z80/.tap/.trd. load_labels and load_listing take sjasmplus "
	"symbol and listing files; afterwards every address argument accepts a label name, and "
	"step_line/run_to_line/source_at work on source lines.\n"
	"3. Most effects precalculate for seconds before drawing. Run once with "
	"stop_pc at the main loop, then save_snapshot, and load that snapshot for every later "
	"measurement rather than paying for the precalculation again. skip_until does the same "
	"inside one call.\n"
	"\n"
	"Which tool answers which question:\n"
	"- what is on screen: screenshot (a PNG to open and look at), screen_text, screen_attrs\n"
	"- did my change alter the DATA: screen_digest, which hashes screen memory\n"
	"- did my change alter the PICTURE: frame_digest, which hashes the frame as the ULA drew "
	"it. These are different questions whenever timing matters: in multicolour the picture "
	"depends on screen memory AND on when the bank is switched relative to the beam, so "
	"drifted timing repaints the screen while screen_digest does not move at all.\n"
	"- does a frame fit, and what is the headroom: frame_cost\n"
	"- where do the T-states go: profile, and disassemble with t_states for exact counts\n"
	"- what did the code do: set_breakpoint, step/step_over/step_out, trace, read_memory\n"
	"- when in the frame does this code run, and is that stable across frames: beam_log\n"
	"- put the machine at a raster position: run_to_beam\n"
	"Every stop already reports where the beam was, so raster work rarely needs a separate "
	"beam_position call.\n"
	"\n"
	"Cost: run/run_frames/step take budgets and are cheap. record_video and audio_capture "
	"run many frames and are not; prefer a snapshot plus frame_cost or screen_digest when "
	"measuring, and record a video to show a result rather than to find one.";

// Every address argument goes through argAddr(), so they all take the same things.
static const char* kAddrArg = "address: a number, \"$hex\"/\"0x\"/\"#hex\", or the name of a loaded label";

// The ceiling on anything measured in frames. It is not a performance limit -
// it is what keeps a mistyped argument from turning into a call that never
// comes back. A million frames is around five and a half hours of emulated
// time, and about half an hour of real time on the machine this was measured
// on, so it is well past any honest use and well short of forever.
static const int kMaxFrames = 1000000;

static Machine g_mach;
static int g_shotCounter = 0;
static int g_videoCounter = 0;

// ------------------------------------------------------------ arg helpers

// A number: a JSON number, or a string in any of the usual notations. Present but
// unreadable is an error, never a silent fallback to the default - a typo that
// quietly becomes zero is worse than a refusal.
static bool argNum(const json& a, const char* key, int& out) {
	if (!a.is_object() || !a.contains(key) || a[key].is_null()) return false;
	const json& v = a[key];
	if (v.is_number_integer()) { out = v.get<int>(); return true; }
	if (v.is_number_float()) { out = (int)v.get<double>(); return true; }
	if (v.is_string() && xsp::parseNumber(v.get<std::string>(), out)) return true;
	throw std::runtime_error(std::string("bad ") + key + ": " + v.dump() + " is not a number");
}

static int argNumOr(const json& a, const char* key, int def) {
	int v = def;
	argNum(a, key, v);
	return v;
}

// The same, but with an interval the value has to be inside. Out of range is a
// refusal rather than a clamp, for the reason above: an argument the caller
// cannot have meant is a mistake worth reporting, and quietly moving it answers
// a question nobody asked. The default is trusted - it is ours.
static int argNumRange(const json& a, const char* key, int def, int lo, int hi) {
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
static bool argAddr(const json& a, const char* key, int& out) {
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
	if (g_mach.labels().find(s, n)) { out = n; return true; }
	throw std::runtime_error("no such label '" + s + "' (" + key + " takes a number, $hex or a "
				 "label name; " + (g_mach.labels().size()
					? std::to_string(g_mach.labels().size()) + " labels loaded from " +
					  g_mach.labels().source()
					: std::string("no symbol table loaded - see load_labels")) + ")");
}

// An address that is going to index something 64K long, so it has to be one.
// Everything that walks a range of memory takes its ends through here: the
// Z80 cannot reach anything else, and a number that is not an address has
// nowhere sensible to be clamped to.
static int argAddrRange(const json& a, const char* key, int def) {
	int v = def;
	if (!argAddr(a, key, v)) return def;
	if (v < 0 || v > 0xffff)
		throw std::runtime_error(std::string("bad ") + key + ": " + std::to_string(v) +
					 " is outside the address space ($0000..$FFFF)");
	return v;
}

// As argAddr(), but also reports which page the address belongs to when the
// argument carries one: a label loaded from a "BB:OOOO name" line, or the same
// "bank:offset" written out by hand (both parts hex, as in the symbol file).
// bank stays -1 for a plain address, which means "wherever it is mapped now".
static bool argAddrBank(const json& a, const char* key, int& out, int& bank) {
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
		if (const auto* e = g_mach.labels().entry(s)) {
			out = e->address;
			bank = e->bank;
			return true;
		}
	}
	return argAddr(a, key, out);
}

static std::string argStr(const json& a, const char* key, const std::string& def = "") {
	if (!a.is_object() || !a.contains(key) || !a[key].is_string()) return def;
	return a[key].get<std::string>();
}

static bool argBool(const json& a, const char* key, bool def) {
	if (!a.is_object() || !a.contains(key) || !a[key].is_boolean()) return def;
	return a[key].get<bool>();
}

// One byte written as bare hex, the way "3E 05 C9" spells it. strtol used to do
// this and took "GG" for zero and "1234" for a byte, so a typo turned into a
// pattern that searches for something else and finds it - or writes it.
static unsigned char parseHexByte(const std::string& tok, const char* key) {
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

static std::string hex16(int v) {
	char buf[16];
	snprintf(buf, sizeof(buf), "$%04X", v & 0xffff);
	return buf;
}

// Where one "frame" ends. A hardware frame is an interrupt, but an effect frame
// is usually the code between two HALTs, and when a render overruns it spans
// several interrupts - so that is the default.
struct SyncSpec {
	bool byInterrupt = false;
	int pc = -1;			// -1: the CPU entering HALT
	std::string text = "halt";
};

static SyncSpec argSync(const json& a, const char* key = "sync") {
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
static json skipUntil(const json& a, const char* key = "skip_until") {
	int adr = 0;
	if (!argAddr(a, key, adr)) return json();
	const long long budget = argNumOr(a, "skip_max_instructions", 200000000);
	xsp::RunResult r = g_mach.run(budget, adr, -1);
	if (r.reason != "pc")
		throw std::runtime_error("never reached " + hex16(adr) + " in " +
					 std::to_string(budget) + " instructions (stopped: " +
					 r.reason + " at " + hex16(r.pc) + ")");
	const int nsPerT = g_mach.comp()->nsPerTick;
	return json{{"address", adr}, {"address_hex", hex16(adr)},
		    {"instructions", r.instructions},
		    {"t_states", nsPerT > 0 ? r.ns / nsPerT : 0},
		    {"frames", r.frames}};
}

static json runToJson(const xsp::RunResult& r) {
	// T-states are the currency on a Z80, and they cannot be recovered from ns
	// afterwards: the core's nanosecond is a truncated integer (284 for 285.714),
	// so only the division it did itself gives the count back exactly.
	const int nsPerT = g_mach.comp() ? g_mach.comp()->nsPerTick : 0;
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

// ------------------------------------------------------------ tool table

struct Tool {
	std::string name;
	std::string description;
	json schema;
	std::function<json(const json&)> fn;
};

static std::vector<Tool> g_tools;

static void tool(const char* name, const char* desc, json schema,
		 std::function<json(const json&)> fn) {
	if (!schema.contains("type")) schema["type"] = "object";
	if (!schema.contains("properties")) schema["properties"] = json::object();
	g_tools.push_back(Tool{name, desc, schema, fn});
}

// The core keeps time in whole nanoseconds: nsPerTick is 1000/cpuFrq truncated and
// then forced even (3.5 MHz -> 284 instead of 285.714). T-states per frame and per
// line are exact, so the honest frame rate is derived from them and the clock instead.
// The core used to carry a nominal `fps` beside them, set to 50 for every ZX machine
// whatever its geometry; upstream deleted it, and `fps_real` is what it should have been.
struct Timing {
	int tPerFrame = 0;		// exact, from the core's own dot timing
	int tPerLine = 0;
	double nsPerT = 0.0;		// real, from the CPU clock
	double nsPerFrame = 0.0;
	double fps = 0.0;
};

static Timing timing(Computer* c) {
	Timing t;
	if (c->nsPerTick > 0) {
		t.tPerFrame = c->vid->nsPerFrame / c->nsPerTick;
		t.tPerLine = c->vid->nsPerLine / c->nsPerTick;
	}
	if (c->cpuFrq > 0) {
		t.nsPerT = 1000.0 / c->cpuFrq;			// cpuFrq is in MHz
		t.nsPerFrame = t.nsPerT * t.tPerFrame;
		if (t.tPerFrame > 0) t.fps = c->cpuFrq * 1e6 / t.tPerFrame;
	}
	return t;
}

// The raster belongs to the geometry, not to the model. Xpeccy keeps it in the
// profile (xcore/profiles.cpp:prfSetLayout) and its built-in "default" layout is
// 448x320 (xcore/config.cpp:104), which is Pentagon's. Naming a model therefore
// does not change the frame length: ask for a ZX48K on the default geometry and
// every timing tool answers 71680 T where the machine's own raster is 69888 - a
// 2.5% error that looks exactly like a real reading.
//
// We report it rather than override it, because the machine you get should stay
// the machine the GUI would have booted. 0 means "we do not claim to know this
// one's raster", and nothing is said.
static int canonicalFrameT(const std::string& model) {
	if (model == "ZX48K" || model == "Scorpion") return 69888;		// 312 lines x 224 T
	if (model == "Spectrum +2" || model == "Spectrum +3") return 70908;	// 311 x 228
	if (model == "Pentagon" || model == "Pentagon1024SL") return 71680;	// 320 x 224
	return 0;
}

static std::string geometryWarning(const Timing& tm) {
	int want = canonicalFrameT(g_mach.model());
	if (!want || !tm.tPerFrame || want == tm.tPerFrame) return "";
	char buf[512];
	snprintf(buf, sizeof(buf),
		 "geometry '%s' gives %d T per frame, but a %s runs %d - timings here are "
		 "%+.1f%% out. The raster comes from the geometry, not from the model: name one "
		 "whose height matches (machine_config {\"geometry\": ...}), or read these numbers "
		 "as the geometry's rather than the machine's.",
		 g_mach.layout().c_str(), tm.tPerFrame, g_mach.model().c_str(), want,
		 100.0 * (tm.tPerFrame - want) / (double)want);
	return buf;
}

// The disk controller the machine is wired with. Worth reporting: a ZX machine without one
// cannot boot TR-DOS at all, and the failure looks like a bad ROM rather than a missing drive.
static const char* diskInterfaceName(int type) {
	switch (type) {
		case DIF_BDI: return "Beta Disk (TR-DOS)";
		case DIF_P3DOS: return "+3 uPD765";
		case DIF_PC: return "PC";
		case DIF_SMK512: return "SMK512";
		default: return "none";
	}
}

static json machineStateJson() {
	Computer* c = g_mach.comp();
	Timing tm = timing(c);
	json pages = json::array();
	for (int a = 0; a < 0x10000; a += 0x4000) {
		xAdr xa = mem_get_xadr(c->mem, a);
		pages.push_back({
			{"cpu_address", hex16(a)},
			{"type", xa.type == MEM_ROM ? "rom" : (xa.type == MEM_RAM ? "ram" : "other")},
			{"bank", xa.bank}
		});
	}
	json out = {
		{"model", g_mach.model()},
		{"memory_kb", g_mach.memoryKb()},
		{"romset", g_mach.romset()},
		{"geometry", g_mach.layout()},
		{"cpu", c->cpu->core ? c->cpu->core->name : "?"},
		{"cpu_frq_mhz", c->cpuFrq},
		{"fps_real", tm.fps},			// clock / T-states per frame
		{"t_states_per_frame", tm.tPerFrame},
		{"pc", cpu_get_pc(c->cpu)},
		{"pc_hex", hex16(cpu_get_pc(c->cpu))},
		{"t_states_frame", c->frmtCount},
		{"screen", {{"width", c->vid->vsze.x}, {"height", c->vid->vsze.y}}},
		{"disk_interface", diskInterfaceName(c->dif->type)},
		{"memory_map", pages}
	};
	std::string warn = geometryWarning(tm);
	if (!warn.empty()) out["warning"] = warn;
	return out;
}

static void registerTools() {

	// -------------------------------------------------- machine

	tool("machine_config",
	     "Read or change the machine. Give a model name (see list_models) and everything else "
	     "- romset, RAM size, screen geometry - comes from the emulator's own defaults for it. "
	     "No arguments = report the current configuration. Changing the model resets the machine.",
	     json{{"properties", {
		     {"model", {{"type", "string"}, {"description", "machine name, e.g. Pentagon, ZX48K, Scorpion"}}},
		     {"memory", {{"type", "integer"}, {"description", "RAM in KB; clamped to what the model supports"}}},
		     {"romset", {{"type", "string"}, {"description", "romset name from the emulator config"}}},
		     {"geometry", {{"type", "string"}, {"description", "screen layout name, e.g. default, Pentagon"}}},
		     {"boot_frames", {{"type", "integer"}, {"description", "frames to run after the reset (default 100)"}}}
	     }}},
	     [](const json& a) {
		     json changed = json::array();
		     std::string err;
		     bool touched = false;
		     if (a.contains("model")) {
			     if (!g_mach.setModel(argStr(a, "model"), err)) throw std::runtime_error(err);
			     changed.push_back("model"); touched = true;
		     }
		     if (a.contains("memory")) {
			     if (!g_mach.setMemory(argNumOr(a, "memory", 128), err)) throw std::runtime_error(err);
			     changed.push_back("memory"); touched = true;
		     }
		     if (a.contains("romset")) {
			     if (!g_mach.setRomset(argStr(a, "romset"), err)) throw std::runtime_error(err);
			     changed.push_back("romset"); touched = true;
		     }
		     if (a.contains("geometry")) {
			     if (!g_mach.setLayout(argStr(a, "geometry"), err)) throw std::runtime_error(err);
			     changed.push_back("geometry"); touched = true;
		     }
		     json res = machineStateJson();
		     res["changed"] = changed;
		     res["config_source"] = g_mach.env().note;
		     if (touched) {
			     g_mach.reset(RES_DEFAULT);
			     int boot = argNumOr(a, "boot_frames", 100);
			     if (boot > 0) res["boot"] = runToJson(g_mach.runFrames(boot));
			     res["pc"] = cpu_get_pc(g_mach.comp()->cpu);
			     res["pc_hex"] = hex16(cpu_get_pc(g_mach.comp()->cpu));
		     }
		     return res;
	     });

	tool("list_models",
	     "All machine names this build of the emulator core can be set to, plus the romsets and "
	     "screen geometries defined in the emulator's configuration.",
	     json{},
	     [](const json&) {
		     json romsets = json::array();
		     for (const auto& r : g_mach.env().romsets) romsets.push_back(r.name);
		     json layouts = json::array();
		     for (const auto& l : g_mach.env().layouts) layouts.push_back(l.name);
		     return json{{"models", g_mach.models()}, {"romsets", romsets}, {"geometries", layouts}};
	     });

	tool("machine_state",
	     "Current machine: model, RAM, romset, CPU, frame timing, screen size and the 16K page map "
	     "(which ROM/RAM bank is visible at $0000/$4000/$8000/$C000).",
	     json{},
	     [](const json&) { return machineStateJson(); });

	tool("reset",
	     "Reset the machine. mode: default, 48, 128, dos, shadow. Runs boot_frames afterwards "
	     "(default 100) so the machine reaches its prompt.",
	     json{{"properties", {
		     {"mode", {{"type", "string"}}},
		     {"boot_frames", {{"type", "integer"}}}
	     }}},
	     [](const json& a) {
		     std::string m = argStr(a, "mode", "default");
		     int mode = RES_DEFAULT;
		     if (m == "48") mode = RES_48;
		     else if (m == "128") mode = RES_128;
		     else if (m == "dos") mode = RES_DOS;
		     else if (m == "shadow") mode = RES_SHADOW;
		     g_mach.reset(mode);
		     int boot = argNumOr(a, "boot_frames", 100);
		     json res{{"mode", m}};
		     if (boot > 0) res["boot"] = runToJson(g_mach.runFrames(boot));
		     res["pc"] = cpu_get_pc(g_mach.comp()->cpu);
		     res["pc_hex"] = hex16(cpu_get_pc(g_mach.comp()->cpu));
		     return res;
	     });

	// -------------------------------------------------- execution

	tool("run",
	     "Run the CPU until something stops it: a breakpoint, stop_pc, max_frames or the "
	     "max_instructions budget. Video and interrupts stay live, so this is safe for code that "
	     "waits for the raster. Returns the stop reason.",
	     json{{"properties", {
		     {"max_instructions", {{"type", "integer"}, {"description", "budget, default 10000000"}}},
		     {"stop_pc", {{"type", "string"}, {"description", "stop when PC reaches this address; label names work"}}},
		     {"max_frames", {{"type", "integer"}, {"description", "stop after this many frames"}}}
	     }}},
	     [](const json& a) {
		     // A budget below 1 disables every stop condition in execLoop and
		     // the call never returns. The server reads the next message only
		     // after the current one is answered, so that is not a slow tool
		     // call - it is a server that stops responding.
		     long long budget = argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff);
		     int stopPc = -1;
		     argAddr(a, "stop_pc", stopPc);
		     int frames = argNumRange(a, "max_frames", -1, 1, kMaxFrames);
		     return runToJson(g_mach.run(budget, stopPc, frames));
	     });

	tool("run_frames",
	     "Run exactly N timing-accurate frames (interrupts + video). Use this to let an effect "
	     "settle before a screenshot: one frame is 1/50 s of emulated time.",
	     json{{"properties", {{"count", {{"type", "integer"},
					     {"minimum", 1}, {"maximum", kMaxFrames},
					     {"description", "frames, default 1"}}}}}},
	     [](const json& a) {
		     return runToJson(g_mach.runFrames(argNumRange(a, "count", 1, 1, kMaxFrames)));
	     });

	tool("run_to_beam",
	     "Run until the video beam reaches a raster position, which is how raster code is "
	     "stepped: by where the picture is being drawn rather than by address or instruction "
	     "count. Give a line, and optionally a dot within it. A target already behind the beam "
	     "means the next frame, not an immediate stop - so calling this repeatedly with the same "
	     "line walks the same point of the raster frame after frame. Stops on the first "
	     "instruction that reached or passed the target and reports where it actually landed, "
	     "since the beam only moves between instructions and lands a few T-states past it.",
	     json{{"properties", {
		     {"line", {{"type", "integer"}, {"description", "raster line in the full frame (see beam_position)"}}},
		     {"dot", {{"type", "integer"}, {"description", "dot within the line; omitted means anywhere on it"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "budget, default 10000000"}}}
	     }}, {"required", json::array({"line"})}},
	     [](const json& a) {
		     Video* v = g_mach.comp()->vid;
		     const int line = argNumRange(a, "line", -1, 0, v->full.y > 0 ? v->full.y - 1 : 0xffff);
		     if (line < 0) throw std::runtime_error("line required");
		     const int dot = argNumRange(a, "dot", -1, 0, v->full.x > 0 ? v->full.x - 1 : 0xffff);
		     long long budget = argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff);
		     json j = runToJson(g_mach.runToBeam(line, dot, budget));
		     j["target"] = {{"line", line}, {"dot", dot}};
		     if (j.contains("beam")) {
			     // how far past the target we actually stopped, in dots and lines
			     const int gotLine = j["beam"]["line"].get<int>();
			     const int gotDot = j["beam"]["dot"].get<int>();
			     j["overshoot_lines"] = gotLine - line;
			     if (dot >= 0 && gotLine == line) j["overshoot_dots"] = gotDot - dot;
		     }
		     return j;
	     });

	tool("step",
	     "Execute N instructions, ignoring breakpoints. Use step_over instead when the next "
	     "instruction is a CALL you do not want to walk through, and step_out to finish the "
	     "routine you are in.",
	     json{{"properties", {{"count", {{"type", "integer"}, {"minimum", 1}}}}}},
	     [](const json& a) { return runToJson(g_mach.step(argNumRange(a, "count", 1, 1, 0x7fffffff))); });

	tool("step_over",
	     "Execute one instruction, but run CALL/RST/block instructions to completion. Gives up "
	     "after max_instructions if the call never comes back.",
	     json{{"properties", {{"max_instructions", {{"type", "integer"},
						       {"description", "budget, default 10000000"}}}}}},
	     [](const json& a) {
		     return runToJson(g_mach.stepOver(argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff)));
	     });

	tool("step_out",
	     "Run until the current subroutine returns (SP rises above its current value). Gives up "
	     "after max_instructions if it never does.",
	     json{{"properties", {{"max_instructions", {{"type", "integer"},
						       {"description", "budget, default 10000000"}}}}}},
	     [](const json& a) {
		     return runToJson(g_mach.stepOut(argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff)));
	     });

	// -------------------------------------------------- cpu / memory

	tool("get_registers",
	     "All CPU registers (main and shadow set), the flags string, IM/IFF and the T-state "
	     "counter for the current frame.",
	     json{},
	     [](const json&) {
		     Computer* c = g_mach.comp();
		     xRegBunch rb = cpuGetRegs(c->cpu);
		     json regs = json::object();
		     // The bunch ends at the first REG_EOT. Stopping on a NULL name instead
		     // reads uninitialised stack: cpuGetRegs() fills only as many entries as
		     // the CPU has registers and marks the end by setting `id` alone, leaving
		     // that entry's `name` whatever was on the stack (cpu/cpu.c).
		     for (int i = 0; i < 32 && rb.regs[i].id != REG_EOT; i++)
			     if (rb.regs[i].name) regs[rb.regs[i].name] = rb.regs[i].value;
		     return json{
			     {"registers", regs},
			     {"flags", rb.flags ? rb.flags : ""},
			     {"flags_value", cpu_get_flag(c->cpu)},
			     {"pc", cpu_get_pc(c->cpu)},
			     {"pc_hex", hex16(cpu_get_pc(c->cpu))},
			     {"sp", cpu_get_sp(c->cpu)},
			     {"halted", c->cpu->flgHALT ? true : false},
			     {"t_states_frame", c->frmtCount}
		     };
	     });

	tool("set_register",
	     "Set one register by name (PC, SP, AF, BC, DE, HL, IX, IY, A, B, ... as reported by get_registers).",
	     json{{"properties", {
		     {"name", {{"type", "string"}}},
		     {"value", {{"type", "string"}, {"description", "number, $hex/0x/#, or a label name"}}}
	     }}, {"required", json::array({"name", "value"})}},
	     [](const json& a) {
		     std::string name = argStr(a, "name");
		     int val = 0;
		     if (!argAddr(a, "value", val)) throw std::runtime_error("value required");
		     if (!cpu_set_reg(g_mach.comp()->cpu, name.c_str(), val))
			     throw std::runtime_error("unknown register '" + name + "'");
		     return json{{"name", name}, {"value", val}, {"ok", true}};
	     });

	tool("read_memory",
	     "Read bytes as the CPU sees them (through the current page mapping). Returns decimal "
	     "bytes plus a hex string.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"length", {{"type", "integer"}, {"description", "default 16, max 16384"}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     int adr = 0;
		     if (!argAddr(a, "address", adr)) throw std::runtime_error("address required");
		     int len = argNumOr(a, "length", 16);
		     if (len < 1) len = 1;
		     if (len > 16384) len = 16384;
		     json bytes = json::array();
		     std::string hex;
		     char buf[8];
		     for (int i = 0; i < len; i++) {
			     int b = g_mach.readByte(adr + i);
			     bytes.push_back(b);
			     snprintf(buf, sizeof(buf), "%02X", b);
			     if (i) hex += ' ';
			     hex += buf;
		     }
		     return json{{"address", adr}, {"address_hex", hex16(adr)}, {"bytes", bytes}, {"hex", hex}};
	     });

	tool("write_memory",
	     "Write bytes through the CPU mapping. Writes to a ROM page are silently dropped by the "
	     "hardware - read back if it matters.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"bytes", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
		     {"hex", {{"type", "string"}, {"description", "alternative to bytes: \"00 3E 05\""}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     int adr = 0;
		     if (!argAddr(a, "address", adr)) throw std::runtime_error("address required");
		     std::vector<int> data;
		     if (a.contains("bytes") && a["bytes"].is_array()) {
			     for (const auto& b : a["bytes"]) {
				     if (!b.is_number_integer() || b.get<int>() < 0 || b.get<int>() > 255)
					     throw std::runtime_error("bad bytes: " + b.dump() +
								      " is not a byte (0..255)");
				     data.push_back(b.get<int>());
			     }
		     } else if (a.contains("hex")) {
			     for (const auto& t : xsp::split(argStr(a, "hex"), ' ')) {
				     std::string s = xsp::trim(t);
				     if (s.empty()) continue;
				     data.push_back(parseHexByte(s, "hex"));
			     }
		     } else {
			     throw std::runtime_error("give bytes[] or hex");
		     }
		     for (size_t i = 0; i < data.size(); i++)
			     g_mach.writeByte(adr + (int)i, data[i]);
		     return json{{"address", adr}, {"written", (int)data.size()}};
	     });

	tool("find_bytes",
	     "Search the CPU address space for a hex pattern (\"3E 05 C9\"). Returns the first "
	     "address or -1. Useful for finding an unlabelled routine by its opcodes, or for "
	     "locating data whose address moved after a rebuild.",
	     json{{"properties", {
		     {"pattern", {{"type", "string"}}},
		     {"from", {{"type", "string"}, {"description", kAddrArg}}},
		     {"to", {{"type", "string"}, {"description", kAddrArg}}}
	     }}, {"required", json::array({"pattern"})}},
	     [](const json& a) {
		     std::vector<unsigned char> pat;
		     for (const auto& t : xsp::split(argStr(a, "pattern"), ' ')) {
			     std::string s = xsp::trim(t);
			     if (!s.empty()) pat.push_back(parseHexByte(s, "pattern"));
		     }
		     if (pat.empty()) throw std::runtime_error("pattern is empty");
		     int from = argAddrRange(a, "from", 0);
		     int to = argAddrRange(a, "to", 0xffff);
		     int at = g_mach.findBytes(pat, from, to);
		     return json{{"address", at}, {"address_hex", at >= 0 ? hex16(at) : "-"}, {"found", at >= 0}};
	     });

	// -------------------------------------------------- code

	tool("disassemble",
	     "Disassemble from an address (default: PC). Each line gives the address, text, length, "
	     "raw bytes and, for jumps and calls, the target address.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", "default: current PC; label names work"}}},
		     {"count", {{"type", "integer"}, {"description", "instructions, default 16"}}},
		     {"t_states", {{"type", "boolean"}, {"description",
				   "add the T-states of each instruction and the total, default false"}}}
	     }}},
	     [](const json& a) {
		     int adr = cpu_get_pc(g_mach.comp()->cpu);
		     argAddr(a, "address", adr);
		     int count = argNumOr(a, "count", 16);
		     if (count < 1) count = 1;
		     if (count > 256) count = 256;
		     const bool wantT = argBool(a, "t_states", false);
		     long long tMax = 0, tMin = 0;
		     bool anyCond = false;
		     json lines = json::array();
		     for (int i = 0; i < count; i++) {
			     int len = 0;
			     xMnem mn;
			     std::string text = g_mach.disasm(adr, &len, &mn);
			     if (len <= 0) len = 1;
			     std::string hex;
			     char buf[8];
			     for (int b = 0; b < len; b++) {
				     snprintf(buf, sizeof(buf), "%02X", g_mach.readByte(adr + b));
				     if (b) hex += ' ';
				     hex += buf;
			     }
			     json line{
				     {"address", adr},
				     {"address_hex", hex16(adr)},
				     {"text", text},
				     {"length", len},
				     {"bytes", hex}
			     };
			     if (const std::string* lab = g_mach.labels().atAddress(adr))
				     line["label"] = *lab;
			     if (const auto* src = g_mach.listing().atAddress(adr))
				     line["source"] = src->text;
			     if (mn.oadr >= 0) {
				     line["target"] = mn.oadr;
				     if (const std::string* lab = g_mach.labels().atAddress(mn.oadr))
					     line["target_label"] = *lab;
			     }
			     if (mn.cond) line["conditional"] = true;
			     if (wantT) {
				     xsp::InstrTiming t = g_mach.instrTiming(adr);
				     line["t_states"] = t.taken;
				     if (t.conditional) {
					     line["t_states_not_taken"] = t.not_taken;
					     line["t_states_text"] = std::to_string(t.taken) + "/" +
								     std::to_string(t.not_taken);
					     anyCond = true;
				     }
				     tMax += t.taken;
				     tMin += t.not_taken;
			     }
			     lines.push_back(line);
			     adr = (adr + len) & 0xffff;
		     }
		     json res{{"lines", lines}, {"next_address", adr}};
		     if (wantT) {
			     res["t_states_total"] = tMax;
			     if (anyCond) {
				     res["t_states_total_min"] = tMin;
				     res["note"] = "totals assume every conditional instruction "
						   "branches (total) or falls through (total_min); "
						   "straight-line code has one number";
			     }
		     }
		     return res;
	     });

	tool("assemble",
	     "Assemble Z80 mnemonics and (by default) write them to memory - write code as text "
	     "instead of hand-assembled bytes. Numbers may be decimal, 0x, $ or # (\"ld hl,$4000\"). "
	     "Define labels with \"loop:\" and jump to them by name, forwards or backwards "
	     "(\"djnz loop\", \"jr done\"); names from load_labels work too, and the labels this "
	     "block defined come back in `labels`. Directives are not supported. "
	     "Stops at the first line that fails.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"lines", {{"type", "array"}, {"items", {{"type", "string"}}}}},
		     {"text", {{"type", "string"}, {"description", "alternative to lines: newline-separated source"}}},
		     {"write", {{"type", "boolean"}, {"description", "write to memory, default true"}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     int adr = 0;
		     if (!argAddr(a, "address", adr)) throw std::runtime_error("address required");
		     std::vector<std::string> lines;
		     if (a.contains("lines") && a["lines"].is_array()) {
			     for (const auto& l : a["lines"]) lines.push_back(l.get<std::string>());
		     } else if (a.contains("text")) {
			     lines = xsp::split(argStr(a, "text"), '\n');
		     } else {
			     throw std::runtime_error("give lines[] or text");
		     }
		     std::map<std::string, int> defined;
		     auto res = g_mach.assemble(adr, lines, argBool(a, "write", true), &defined);
		     json out = json::array();
		     int next = adr;
		     bool ok = true;
		     for (const auto& l : res) {
			     json hexBytes = json::array();
			     std::string hex;
			     char buf[8];
			     for (size_t i = 0; i < l.bytes.size(); i++) {
				     snprintf(buf, sizeof(buf), "%02X", l.bytes[i]);
				     if (i) hex += ' ';
				     hex += buf;
				     hexBytes.push_back(l.bytes[i]);
			     }
			     out.push_back({{"address", l.address}, {"address_hex", hex16(l.address)},
					    {"text", l.text}, {"bytes", hexBytes}, {"hex", hex},
					    {"ok", l.ok}, {"error", l.error}});
			     if (l.ok) next = l.address + (int)l.bytes.size();
			     else ok = false;
		     }
		     json labels = json::object();
		     for (const auto& kv : defined)
			     labels[kv.first] = {{"address", kv.second}, {"address_hex", hex16(kv.second)}};
		     return json{{"lines", out}, {"next_address", next}, {"ok", ok},
			     {"labels", labels}};
	     });

	// -------------------------------------------------- breakpoints

	tool("set_breakpoint",
	     "Break when the CPU touches an address. access: exec (default), read, write, or any "
	     "combination like \"read,write\". scope: \"cell\" (default) puts the breakpoint in the "
	     "memory cell, so it follows the bank; \"address\" puts it on the CPU address, so it "
	     "fires whatever is paged in there. A banked label (\"05:200E main_loop\") or an explicit "
	     "\"bank:offset\" arms that bank's cell even when the bank is not paged in right now - "
	     "without one, cell scope can only mean whatever is mapped at the time.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"access", {{"type", "string"}, {"description", "exec | read | write | \"read,write\""}}},
		     {"scope", {{"type", "string"}, {"description", "cell (default) | address"}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     int adr = 0, bank = -1;
		     if (!argAddrBank(a, "address", adr, bank)) throw std::runtime_error("address required");
		     std::string acc = argStr(a, "access", "exec");
		     unsigned char flags = 0;
		     if (acc.find("exec") != std::string::npos) flags |= MEM_BRK_FETCH;
		     if (acc.find("read") != std::string::npos) flags |= MEM_BRK_RD;
		     if (acc.find("write") != std::string::npos) flags |= MEM_BRK_WR;
		     if (!flags) flags = MEM_BRK_FETCH;

		     std::string scope = argStr(a, "scope", "cell");
		     if (scope != "cell" && scope != "address")
			     throw std::runtime_error("scope must be \"cell\" or \"address\", not '" + scope + "'");

		     json out{{"address", adr}, {"address_hex", hex16(adr)},
			     {"access", acc}, {"flags", flags}, {"scope", scope}, {"ok", true}};
		     if (scope == "address") {
			     g_mach.setBreakAddress(adr, flags);
		     } else if (bank >= 0) {
			     g_mach.setBreakBank(bank, adr, flags);
			     out["bank"] = bank;
			     out["bank_paged_in"] = (g_mach.bankAt(adr) == bank);
		     } else {
			     g_mach.setBreak(adr, flags);
			     out["bank"] = g_mach.bankAt(adr);
			     out["phys_address"] = g_mach.physAddress(adr);
		     }
		     return out;
	     });

	tool("clear_breakpoints",
	     "Remove every breakpoint set through this server (memory, address and I/O).",
	     json{},
	     [](const json&) { g_mach.clearBreaks(); return json{{"ok", true}}; });

	tool("set_port_breakpoint",
	     "Break on an I/O port access. access: read, write or both. This is how paging and "
	     "border writes are caught without knowing where in the code they happen: $7FFD for "
	     "the 128K pager, $FE for border and beeper.",
	     json{{"properties", {
		     {"port", {{"type", "string"}, {"description", "port number, or a label/EQU name"}}},
		     {"access", {{"type", "string"}}}
	     }}, {"required", json::array({"port"})}},
	     [](const json& a) {
		     int port = 0;
		     if (!argAddr(a, "port", port)) throw std::runtime_error("port required");
		     std::string acc = argStr(a, "access", "read,write");
		     unsigned char flags = 0;
		     if (acc.find("read") != std::string::npos) flags |= MEM_BRK_RD;
		     if (acc.find("write") != std::string::npos) flags |= MEM_BRK_WR;
		     if (!flags) flags = MEM_BRK_RD | MEM_BRK_WR;
		     g_mach.setIoBreak(port, flags);
		     return json{{"port", port}, {"access", acc}, {"ok", true}};
	     });

	// -------------------------------------------------- vision

	tool("screenshot",
	     "Write the last completed frame to a PNG file and return its path - the ground truth of "
	     "what is on screen. Open the file to look at it. border=false crops to the 256x192 paper "
	     "area, scale enlarges by pixel doubling.",
	     json{{"properties", {
		     {"path", {{"type", "string"}, {"description", "output file; default: a temp file"}}},
		     {"border", {{"type", "boolean"}, {"description", "include the border, default true"}}},
		     {"scale", {{"type", "integer"}, {"description", "1..8, default 1"}}}
	     }}},
	     [](const json& a) {
		     bool border = argBool(a, "border", true);
		     int scale = argNumOr(a, "scale", 1);
		     std::string path = argStr(a, "path");
		     if (path.empty()) {
			     std::string dir = xsp::platform::join(xsp::platform::tempDir(), "xspeccy-mcp");
			     xsp::platform::makeDir(dir);
			     char buf[32];
			     snprintf(buf, sizeof(buf), "shot-%03d.png", ++g_shotCounter);
			     path = xsp::platform::join(dir, buf);
		     }
		     auto shot = xsp::video::capture(g_mach.comp(), border, scale);
		     if (shot.rgba.empty()) throw std::runtime_error("no frame captured yet - run some frames first");
		     std::string err;
		     if (!xsp::writePng(path, shot.rgba.data(), shot.width, shot.height, err))
			     throw std::runtime_error(err);
		     return json{{"path", path}, {"width", shot.width}, {"height", shot.height},
			     {"border", border}, {"scale", scale}};
	     });

	tool("record_video",
	     "Record the screen while the machine runs. Default is an animated GIF, which needs "
	     "nothing installed and reproduces the ZX palette exactly. Give the path an .mp4/.webm "
	     "extension to encode with ffmpeg instead, which also lets the recording carry the "
	     "machine's real sound. Length is up to you: 100 frames is two seconds, 15000 is five "
	     "minutes - frames are written as they are produced, so long recordings cost no extra "
	     "memory. Use every_nth to halve or quarter the frame rate for smaller files, or "
	     "every_nth:\"auto\" to have it match the effect's own update rate. skip_until winds "
	     "past the precalculation first, so the recording does not open on a black screen.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "emulated frames to record, default 100 (2 s)"}}},
		     {"path", {{"type", "string"}, {"description", "output file; extension picks the format, default .gif"}}},
		     {"every_nth", {{"description", "record every Nth frame, default 1; \"auto\" measures "
				    "the effect's quantum and uses that, so no frame is a duplicate"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before recording - e.g. the main loop, "
				     "to skip the precalculation"}}},
		     {"border", {{"type", "boolean"}, {"description", "include the border, default true"}}},
		     {"scale", {{"type", "integer"}, {"description", "1..8, default 1"}}},
		     {"audio", {{"type", "boolean"}, {"description", "add the sound track (mp4/webm only), default true"}}}
	     }}},
	     [](const json& a) {
		     int frames = argNumOr(a, "frames", 100);
		     if (frames < 1) frames = 1;
		     if (frames > 60000) frames = 60000;		// ~20 minutes
		     json skipped = skipUntil(a);
		     // An effect that needs N interrupts per frame draws the same picture
		     // N times running; recording all of them makes the file N times
		     // bigger and carries no extra pixel. The quantum is measurable, so
		     // measure it rather than asking the caller to know it.
		     json autoNth;
		     int nth = 1;
		     if (a.contains("every_nth") && a["every_nth"].is_string() &&
			 xsp::trim(a["every_nth"].get<std::string>()) == "auto") {
			     Timing t0 = timing(g_mach.comp());
			     const long long budget = 20000000;
			     long long worst = 0;
			     int measured = 0;
			     if (g_mach.frameCost(-1, budget, false).complete) {
				     for (int i = 0; i < 2; i++) {
					     xsp::FrameCost fc = g_mach.frameCost(-1, budget, false);
					     if (!fc.complete) break;
					     const long long w = g_mach.comp()->nsPerTick > 0
						     ? fc.work_ns / g_mach.comp()->nsPerTick : 0;
					     if (w > worst) worst = w;
					     measured++;
				     }
			     }
			     if (measured && t0.tPerFrame > 0) {
				     nth = (int)((worst + t0.tPerFrame - 1) / t0.tPerFrame);
				     if (nth < 1) nth = 1;
				     autoNth = json{{"every_nth", nth}, {"frame_t_states", worst},
						    {"t_states_per_frame", t0.tPerFrame},
						    {"frames_measured", measured}};
			     } else {
				     autoNth = json{{"every_nth", 1},
						    {"note", "no HALT boundary found, so the effect's "
							     "rate could not be measured - recording "
							     "every frame"}};
			     }
		     } else {
			     nth = argNumOr(a, "every_nth", 1);
		     }
		     if (nth < 1) nth = 1;
		     const bool border = argBool(a, "border", true);
		     const int scale = argNumOr(a, "scale", 1);

		     std::string path = argStr(a, "path");
		     if (path.empty()) {
			     std::string dir = xsp::platform::join(xsp::platform::tempDir(), "xspeccy-mcp");
			     xsp::platform::makeDir(dir);
			     char buf[32];
			     snprintf(buf, sizeof(buf), "video-%03d.gif", ++g_videoCounter);
			     path = xsp::platform::join(dir, buf);
		     }
		     std::string ext;
		     size_t dot = path.find_last_of('.');
		     if (dot != std::string::npos) ext = path.substr(dot + 1);
		     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
		     const bool gif = (ext == "gif" || ext.empty());
		     if (!gif && !xsp::record::FfmpegWriter::available())
			     throw std::runtime_error("ffmpeg is needed for ." + ext + " - use a .gif path instead");

		     Computer* c = g_mach.comp();
		     // the rate the machine really runs at, so playback matches wall-clock
		     const double fps = timing(c).fps > 0 ? timing(c).fps : 50.0;
		     const double outFps = fps / nth;
		     const bool withAudio = !gif && argBool(a, "audio", true);

		     // first frame decides the geometry
		     auto shot = xsp::video::capture(c, border, scale);
		     if (shot.rgba.empty()) throw std::runtime_error("no frame to record yet - run some frames first");

		     xsp::record::GifWriter gifw;
		     xsp::record::FfmpegWriter ff;
		     std::string err;
		     std::string videoPath = path;
		     std::string tmpVideo, tmpWav;
		     if (gif) {
			     if (!gifw.open(path, shot.width, shot.height, c->vid->pal, 256, 0, err))
				     throw std::runtime_error(err);
		     } else {
			     if (withAudio) {
				     std::string dir = xsp::platform::join(xsp::platform::tempDir(), "xspeccy-mcp");
				     xsp::platform::makeDir(dir);
				     tmpVideo = xsp::platform::join(dir, "rec-video." + ext);
				     tmpWav = xsp::platform::join(dir, "rec-audio.wav");
				     videoPath = tmpVideo;
			     }
			     if (!ff.open(videoPath, shot.width, shot.height, (int)(outFps + 0.5), err))
				     throw std::runtime_error(err);
		     }

		     if (withAudio) g_mach.audio().start(44100, true, false);

		     const int delayCs = (int)(100.0 / outFps + 0.5);
		     int written = 0;
		     for (int i = 0; i < frames; i++) {
			     g_mach.runFrames(1);
			     if (i % nth) continue;
			     auto f = xsp::video::capture(c, border, scale);
			     if (f.width != shot.width || f.height != shot.height) continue;	// geometry changed mid-recording
			     bool ok = gif ? gifw.addFrame(f.rgba.data(), delayCs, err)
					   : ff.addFrame(f.rgba.data(), f.rgba.size(), err);
			     if (!ok) throw std::runtime_error(err);
			     written++;
		     }

		     if (gif) {
			     gifw.close();
		     } else {
			     ff.close();
			     if (withAudio) {
				     auto& cap = g_mach.audio();
				     cap.stop();
				     if (xsp::audio::writeWav(tmpWav, cap, err)) {
					     if (!xsp::record::FfmpegWriter::mux(tmpVideo, tmpWav, path, err))
						     throw std::runtime_error(err);
				     } else {
					     throw std::runtime_error("audio: " + err);
				     }
			     }
		     }
		     if (withAudio) g_mach.audio().stop();

		     long long size = 0;
		     if (FILE* f = fopen(path.c_str(), "rb")) {
			     fseek(f, 0, SEEK_END);
			     size = ftell(f);
			     fclose(f);
		     }
		     if (!size) throw std::runtime_error("nothing was written to '" + path + "'");

		     json res{
			     {"path", path},
			     {"format", gif ? "gif" : ext},
			     {"frames_recorded", written},
			     {"frames_emulated", frames},
			     {"every_nth", nth},
			     {"fps", outFps},
			     {"duration_s", frames / fps},
			     {"width", shot.width},
			     {"height", shot.height},
			     {"bytes", size},
			     {"audio", withAudio}
		     };
		     if (!skipped.is_null()) res["skipped_to"] = skipped;
		     if (!autoNth.is_null()) res["every_nth_auto"] = autoNth;
		     return res;
	     });

	tool("screen_text",
	     "Decode the ZX screen into 32x24 text by matching each cell against the ROM font. Cheap "
	     "way to read a BASIC listing or a menu. Graphics come out as '?' - for anything visual "
	     "use screenshot, which shows the real pixels.",
	     json{},
	     [](const json&) { return json{{"screen", xsp::video::screenText(g_mach.comp())}}; });

	tool("screen_attrs",
	     "The 32x24 attribute grid as hex (ink/paper/bright/flash per cell). Cheaper to read "
	     "than a screenshot when the question is about colour rather than shape, and unlike a "
	     "PNG it can be compared in the reply itself.",
	     json{},
	     [](const json&) { return json{{"attributes", xsp::video::screenAttrs(g_mach.comp())}}; });

	tool("screen_digest",
	     "A short hash of the screen, per frame - the cheap way to prove an optimisation did not "
	     "change the picture. Run it before a change and after it and compare the lists; identical "
	     "digests mean the output is identical byte for byte. It hashes screen memory in the RAM "
	     "banks rather than the visible frame, and by default both screen banks (5 and 7), because "
	     "double-buffered code writes only one of them per frame and hashing just one flip-flops "
	     "between matching and not. A frame is the code between two HALTs by default (see `sync`), "
	     "which is the effect's own frame even when it overruns the interrupt. Give `from` and "
	     "`to` to hash a stretch of memory instead of the screen - a precalculated table, a "
	     "buffer, anything the picture does not show.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "frames to run and hash, default 1"}}},
		     {"scope", {{"type", "string"}, {"description", "attrs (default, 768 bytes/bank) | screen (pixels+attrs)"}}},
		     {"banks", {{"type", "array"}, {"description", "RAM banks to hash, default [5,7] (or [5] on a 48K)"},
				{"items", {{"type", "integer"}}}}},
		     {"sync", {{"type", "string"}, {"description", "halt (default) | frame | an address/label to stop at"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before hashing anything - e.g. the main "
				     "loop, to get past the precalculation"}}},
		     {"from", {{"type", "string"}, {"description",
			       "hash this CPU address range instead of the screen (with `to`) - a "
			       "table, a buffer, anything the picture does not show"}}},
		     {"to", {{"type", "string"}, {"description", "last address of the range, inclusive"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "per-frame budget, default 20000000"}}}
	     }}},
	     [](const json& a) {
		     int frames = argNumOr(a, "frames", 1);
		     if (frames < 1) frames = 1;
		     if (frames > 10000) frames = 10000;
		     json skipped = skipUntil(a);

		     // an explicit range means "hash this memory", and then the screen
		     // arguments have nothing to describe
		     int from = 0, to = 0;
		     const bool hasFrom = argAddr(a, "from", from);
		     const bool hasTo = argAddr(a, "to", to);
		     if (hasFrom != hasTo)
			     throw std::runtime_error("give both from and to, or neither");
		     const bool byRange = hasFrom;
		     if (byRange && (a.contains("banks") || a.contains("scope")))
			     throw std::runtime_error("from/to hash memory directly: banks and scope "
						      "describe the screen and cannot apply");

		     std::string scope = argStr(a, "scope", "attrs");
		     if (scope != "attrs" && scope != "screen")
			     throw std::runtime_error("scope must be attrs or screen");
		     std::vector<int> banks;
		     if (a.contains("banks") && a["banks"].is_array()) {
			     for (const auto& b : a["banks"]) banks.push_back(b.get<int>());
			     if (banks.empty()) throw std::runtime_error("banks[] is empty");
		     } else if (!byRange) {
			     banks.push_back(5);
			     if (g_mach.memoryKb() >= 128) banks.push_back(7);
		     }
		     SyncSpec sync = argSync(a);
		     const long long budget = argNumOr(a, "max_instructions", 20000000);

		     json list = json::array();
		     std::string err;
		     bool truncated = false;
		     for (int i = 0; i < frames; i++) {
			     if (sync.byInterrupt) {
				     g_mach.runFrames(1);
			     } else {
				     xsp::FrameCost fc = g_mach.frameCost(sync.pc, budget);
				     if (!fc.complete) { truncated = true; }
			     }
			     std::string d = byRange
				     ? xsp::video::memoryDigest(g_mach.comp(), from, to, err)
				     : xsp::video::screenDigest(g_mach.comp(), banks,
								scope == "screen", err);
			     if (d.empty()) throw std::runtime_error(err);
			     list.push_back({{"frame", i}, {"digest", d}});
			     if (truncated) break;
		     }
		     json res{{"sync", sync.text}, {"frames", (int)list.size()}, {"digests", list}};
		     if (byRange) {
			     res["scope"] = "memory";
			     res["from"] = from;
			     res["from_hex"] = hex16(from);
			     res["to"] = to;
			     res["to_hex"] = hex16(to);
			     res["bytes"] = to - from + 1;
		     } else {
			     res["scope"] = scope;
			     res["banks"] = banks;
		     }
		     if (!skipped.is_null()) res["skipped_to"] = skipped;
		     if (truncated)
			     res["warning"] = "no frame boundary within max_instructions - the digest "
					      "list is short; check `sync`";
		     return res;
	     });

	tool("frame_digest",
	     "A short hash of the frame as the ULA actually drew it - the picture, not the memory "
	     "behind it. This is the one to use for multicolour, and it answers a different question "
	     "from screen_digest. screen_digest hashes screen memory; the visible picture is a "
	     "function of that memory AND of when the bank is switched relative to the beam, so raster "
	     "code whose timing has drifted paints a different screen out of byte-identical data and "
	     "screen_digest cannot see it move. If your data hashes stable and the picture still looks "
	     "wrong, that is not a paradox - it is this. `lines` hashes every scanline separately and "
	     "reports which line ranges differ from the previous frame, which turns \"some frames are "
	     "wrong\" into a line number.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "frames to run and hash, default 1"}}},
		     {"border", {{"type", "boolean"}, {"description", "include the border, default true"}}},
		     {"lines", {{"type", "boolean"}, {"description",
				"also hash each scanline and report the ranges that changed"}}},
		     {"sync", {{"type", "string"}, {"description", "halt | frame (default) | an address/label"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before hashing - e.g. past the precalculation"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "per-frame budget, default 20000000"}}}
	     }}},
	     [](const json& a) {
		     int frames = argNumRange(a, "frames", 1, 1, 10000);
		     const bool border = argBool(a, "border", true);
		     const bool perLine = argBool(a, "lines", false);
		     json skipped = skipUntil(a);
		     // A hardware frame by default, unlike screen_digest: the picture is
		     // made by the ULA, and its unit is the interrupt rather than wherever
		     // the effect happens to call its own frame boundary.
		     SyncSpec sync = argSync(a);
		     if (!a.is_object() || !a.contains("sync") || a["sync"].is_null()) {
			     sync.byInterrupt = true;
			     sync.pc = -1;
			     sync.text = "frame";
		     }
		     const long long budget = argNumOr(a, "max_instructions", 20000000);

		     json list = json::array();
		     std::vector<std::string> prevLines;
		     bool truncated = false;
		     int width = 0, height = 0;
		     std::map<std::string, int> seen;	// digest -> first frame that had it
		     for (int i = 0; i < frames; i++) {
			     if (sync.byInterrupt) {
				     g_mach.runFrames(1);
			     } else {
				     xsp::FrameCost fc = g_mach.frameCost(sync.pc, budget);
				     if (!fc.complete) truncated = true;
			     }
			     xsp::video::FrameHash fh =
				     xsp::video::frameDigest(g_mach.comp(), border, perLine);
			     if (fh.digest.empty())
				     throw std::runtime_error("no frame buffer to hash yet - run first");
			     width = fh.width;
			     height = fh.height;
			     json e{{"frame", i}, {"digest", fh.digest}};
			     auto it = seen.find(fh.digest);
			     if (it == seen.end()) seen.emplace(fh.digest, i);
			     else e["same_as_frame"] = it->second;
			     if (perLine) {
				     // Ranges rather than 300 hashes: what is wanted is "which
				     // part of the screen moved", and a list of line numbers is
				     // the same answer spelled out at length.
				     json ranges = json::array();
				     if (!prevLines.empty()) {
					     int run = -1;
					     for (size_t y = 0; y < fh.lines.size(); y++) {
						     const bool diff = y >= prevLines.size() ||
								       fh.lines[y] != prevLines[y];
						     if (diff && run < 0) run = (int)y;
						     if (!diff && run >= 0) {
							     ranges.push_back({run, (int)y - 1});
							     run = -1;
						     }
					     }
					     if (run >= 0)
						     ranges.push_back({run, (int)fh.lines.size() - 1});
				     }
				     e["changed_lines"] = ranges;
				     e["changed_line_count"] = [&]() {
					     int n = 0;
					     for (const auto& rg : ranges)
						     n += rg[1].get<int>() - rg[0].get<int>() + 1;
					     return n;
				     }();
				     prevLines = fh.lines;
			     }
			     list.push_back(e);
			     if (truncated) break;
		     }
		     json res{{"sync", sync.text}, {"frames", (int)list.size()},
			      {"scope", border ? "frame with border" : "paper only"},
			      {"width", width}, {"height", height},
			      {"unique_digests", (int)seen.size()},
			      {"digests", list}};
		     if (!skipped.is_null()) res["skipped_to"] = skipped;
		     if (truncated)
			     res["warning"] = "no frame boundary within max_instructions - the digest "
					      "list is short; check `sync`";
		     return res;
	     });

	// -------------------------------------------------- symbols

	tool("load_labels",
	     "Load a symbol table so addresses have names. Understands the sjasmplus formats "
	     "(\"05:200E name\", \"FF:8000 name\", \"name: EQU $8000\", \"name = 32768\") - the same "
	     "file you would pass to xpeccy with -l. In the bank form the number is an offset inside "
	     "the bank, so bank 5 lands at $4000, bank 2 at $8000 and any other bank at $C000; bank FF "
	     "is a plain CPU address. Beware that sjasmplus writes EQU constants into the same file "
	     "and they are indistinguishable from addresses. Afterwards every address argument accepts "
	     "a label name, disassemble annotates lines and jump targets, and assemble takes labels as "
	     "operands.",
	     json{{"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string err;
		     if (!g_mach.labels().load(argStr(a, "path"), err)) throw std::runtime_error(err);
		     json sample = json::array();
		     json banks = json::object();
		     int n = 0;
		     for (const auto& e : g_mach.labels().all()) {
			     const std::string key = e.bank < 0 ? "cpu" : std::to_string(e.bank);
			     banks[key] = banks.value(key, 0) + 1;
			     if (n++ >= 10) continue;
			     sample.push_back({{"name", e.name}, {"address", e.address},
					       {"address_hex", hex16(e.address)}, {"bank", e.bank}});
		     }
		     return json{{"path", g_mach.labels().source()},
			     {"count", (int)g_mach.labels().size()},
			     {"by_bank", banks}, {"sample", sample}};
	     });

	tool("resolve_symbol",
	     "Look a symbol up by name, or find the label at an address. With neither argument, "
	     "returns the whole table.",
	     json{{"properties", {
		     {"name", {{"type", "string"}}},
		     {"address", {{"type", "string"}, {"description", kAddrArg}}}
	     }}},
	     [](const json& a) {
		     auto& lab = g_mach.labels();
		     if (a.contains("name")) {
			     std::string nm = argStr(a, "name");
			     const auto* e = lab.entry(nm);
			     if (!e) throw std::runtime_error("no such label '" + nm + "'");
			     return json{{"name", nm}, {"address", e->address},
				     {"address_hex", hex16(e->address)}, {"bank", e->bank}};
		     }
		     int adr = 0;
		     if (argAddr(a, "address", adr)) {
			     const std::string* nm = lab.atAddress(adr);
			     return json{{"address", adr}, {"address_hex", hex16(adr)},
				     {"name", nm ? *nm : ""}, {"found", nm != nullptr}};
		     }
		     json all = json::array();
		     for (const auto& e : lab.all())
			     all.push_back({{"name", e.name}, {"address", e.address},
					    {"address_hex", hex16(e.address)}, {"bank", e.bank}});
		     return json{{"count", (int)lab.size()}, {"labels", all}, {"source", lab.source()}};
	     });

	tool("load_listing",
	     "Load an assembler listing (sjasmplus .lst) to debug in terms of source lines instead of "
	     "addresses. Enables source_at, step_line and run_to_line, and makes disassemble show the "
	     "original source for each address.",
	     json{{"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string err;
		     if (!g_mach.listing().load(argStr(a, "path"), err)) throw std::runtime_error(err);
		     return json{{"path", g_mach.listing().source()},
			     {"lines", (int)g_mach.listing().size()}};
	     });

	tool("source_at",
	     "The source line at an address (default: PC), with a few lines of context either side - "
	     "what the programmer wrote, not what the disassembler reconstructs.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"context", {{"type", "integer"}, {"description", "lines each way, default 3"}}}
	     }}},
	     [](const json& a) {
		     int adr = cpu_get_pc(g_mach.comp()->cpu);
		     argAddr(a, "address", adr);
		     int ctx = argNumOr(a, "context", 3);
		     auto lines = g_mach.listing().around(adr, ctx, ctx);
		     if (lines.empty()) throw std::runtime_error("no listing line covers " + hex16(adr));
		     const auto* here = g_mach.listing().coveringAddress(adr);
		     json out = json::array();
		     for (const auto& l : lines)
			     out.push_back({{"address", l.address}, {"address_hex", hex16(l.address)},
					    {"line", l.line}, {"file", l.file}, {"text", l.text},
					    {"current", here && l.line == here->line && l.file == here->file}});
		     return json{{"address", adr}, {"address_hex", hex16(adr)},
			     {"line", here ? here->line : -1}, {"file", here ? here->file : ""},
			     {"source", out}};
	     });

	tool("step_line",
	     "Step until execution reaches a different source line - one source step instead of one "
	     "instruction. Needs a listing (load_listing); without one it falls back to a single step.",
	     json{},
	     [](const json&) {
		     auto r = runToJson(g_mach.stepLine());
		     if (const auto* l = g_mach.listing().coveringAddress(cpu_get_pc(g_mach.comp()->cpu))) {
			     r["line"] = l->line;
			     r["file"] = l->file;
			     r["text"] = l->text;
		     }
		     return r;
	     });

	tool("run_to_line",
	     "Run until a given source line in the listing is reached. Needs load_listing first, "
	     "and the line must be one that generated code - a comment or an equate has no address "
	     "to stop at.",
	     json{{"properties", {
		     {"line", {{"type", "integer"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "budget, default 10000000"}}}
	     }}, {"required", json::array({"line"})}},
	     [](const json& a) {
		     int line = argNumOr(a, "line", 0);
		     int adr = 0;
		     if (!g_mach.listing().addressOfLine(line, adr))
			     throw std::runtime_error("line " + std::to_string(line) + " has no code in the listing");
		     auto r = runToJson(g_mach.run(argNumOr(a, "max_instructions", 10000000), adr, -1));
		     r["target"] = adr;
		     r["target_hex"] = hex16(adr);
		     r["line"] = line;
		     return r;
	     });

	// -------------------------------------------------- coverage

	tool("coverage",
	     "Which bytes of code actually ran. action=start begins recording, read reports the "
	     "executed fraction of a range and - more usefully - the biggest stretches that never "
	     "executed, so you can see which branches your test never took.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "start | stop | read | reset"}}},
		     {"from", {{"type", "string"}, {"description", "range start, default $4000; label names work"}}},
		     {"to", {{"type", "string"}, {"description", "range end, default $FFFF; label names work"}}},
		     {"max_gaps", {{"type", "integer"}, {"description", "unexecuted stretches to list, default 10"}}},
		     {"min_gap", {{"type", "integer"}, {"description", "ignore gaps shorter than this, default 4"}}}
	     }}},
	     [](const json& a) {
		     auto& cov = g_mach.coverage();
		     std::string action = argStr(a, "action", "read");
		     if (action == "start") { cov.reset(); cov.on = true; }
		     else if (action == "stop") cov.on = false;
		     else if (action == "reset") cov.reset();
		     else if (action != "read") throw std::runtime_error("action must be start, stop, read or reset");

		     // Both ends checked before the swap, not after: clamping one end
		     // and then swapping used to walk the map from the surviving end
		     // out to the unchecked one, which reads past the array and
		     // reports a byte count for memory that does not exist.
		     int from = argAddrRange(a, "from", 0x4000);
		     int to = argAddrRange(a, "to", 0xffff);
		     if (to < from) std::swap(from, to);

		     int executed = 0;
		     for (int i = from; i <= to; i++) if (cov.map[i] & 1) executed++;

		     int minGap = argNumOr(a, "min_gap", 4);
		     int maxGaps = argNumOr(a, "max_gaps", 10);
		     std::vector<std::pair<int, int>> gaps;		// (size, start)
		     int gapStart = -1;
		     for (int i = from; i <= to + 1; i++) {
			     bool run = (i <= to) && !(cov.map[i] & 1);
			     if (run && gapStart < 0) gapStart = i;
			     if (!run && gapStart >= 0) {
				     int size = i - gapStart;
				     if (size >= minGap) gaps.push_back({size, gapStart});
				     gapStart = -1;
			     }
		     }
		     std::sort(gaps.begin(), gaps.end(), std::greater<std::pair<int, int>>());
		     json out = json::array();
		     for (int i = 0; i < (int)gaps.size() && i < maxGaps; i++) {
			     json g{{"start", gaps[i].second}, {"start_hex", hex16(gaps[i].second)},
				    {"size", gaps[i].first},
				    {"end_hex", hex16(gaps[i].second + gaps[i].first - 1)}};
			     if (const std::string* lab = g_mach.labels().atAddress(gaps[i].second))
				     g["label"] = *lab;
			     out.push_back(g);
		     }
		     const int total = to - from + 1;
		     return json{
			     {"running", cov.on},
			     {"instructions", cov.instructions},
			     {"range", {{"from", from}, {"to", to}}},
			     {"bytes_executed", executed},
			     {"bytes_total", total},
			     {"percent", total ? 100.0 * executed / total : 0.0},
			     {"unexecuted_gaps", out},
			     {"gaps_total", (int)gaps.size()}
		     };
	     });

	// -------------------------------------------------- raster and timing

	tool("beam_position",
	     "Where the video beam is right now: dot and line inside the full frame, which zone it is "
	     "in (paper, border or blanking), and the T-state count since the interrupt. This is what "
	     "you need when debugging multicolour, border effects or anything raster-timed.",
	     json{},
	     [](const json&) {
		     Computer* c = g_mach.comp();
		     Video* v = c->vid;
		     int x = v->ray.x, y = v->ray.y;
		     std::string zone = "blank";
		     bool inPaperX = x >= v->bord.x && x < v->send.x;
		     bool inPaperY = y >= v->bord.y && y < v->send.y;
		     if (x < v->vend.x && y < v->vend.y)
			     zone = (inPaperX && inPaperY) ? "paper" : "border";
		     Timing tm = timing(c);
		     return json{
			     {"dot", x}, {"line", y}, {"zone", zone},
			     {"paper_x", inPaperX ? x - v->bord.x : -1},
			     {"paper_y", inPaperY ? y - v->bord.y : -1},
			     {"t_states_frame", c->frmtCount},
			     {"t_states_per_frame", tm.tPerFrame},
			     {"t_states_per_line", tm.tPerLine},
			     {"frame_counter", v->fcnt},
			     {"vblank", v->vblank ? true : false},
			     {"hblank", v->hblank ? true : false}
		     };
	     });

	tool("frame_timing",
	     "Frame geometry and timing budget: dots and lines, T-states per line and per frame, "
	     "the paper/border/blanking split, and how many T-states the last frame actually took. "
	     "Budget an effect against t_states_per_frame and fps_real - the clock divided by the "
	     "T-states in a frame, 48.83 Hz on a Pentagon rather than the round 50 it is usually "
	     "quoted at. The ns_per_* fields are the core's internal quantum, which is a truncated "
	     "integer; the *_real ones come from the CPU clock and are what a real machine does.",
	     json{},
	     [](const json&) {
		     Computer* c = g_mach.comp();
		     Video* v = c->vid;
		     Timing tm = timing(c);
		     json out = {
			     {"fps_real", tm.fps},
			     {"cpu_frq_mhz", c->cpuFrq},
			     {"t_states_per_frame", tm.tPerFrame},
			     {"t_states_per_line", tm.tPerLine},
			     {"t_states_last_frame", c->fCount},
			     {"t_states_this_frame", c->frmtCount},
			     {"ns_per_frame", v->nsPerFrame},
			     {"ns_per_frame_real", tm.nsPerFrame},
			     {"ns_per_line", v->nsPerLine},
			     {"ns_per_dot", v->nsPerDot},
			     {"ns_per_t_state", c->nsPerTick},
			     {"ns_per_t_state_real", tm.nsPerT},
			     {"geometry", {
				     {"full", {{"x", v->full.x}, {"y", v->full.y}}},
				     {"blank", {{"x", v->blank.x}, {"y", v->blank.y}}},
				     {"border", {{"x", v->bord.x}, {"y", v->bord.y}}},
				     {"paper", {{"x", v->scrn.x}, {"y", v->scrn.y}}},
				     {"visible", {{"x", v->vsze.x}, {"y", v->vsze.y}}}
			     }},
			     {"interrupt", {{"position", v->intp.y}, {"length_t", v->intsize}}}
		     };
		     std::string warn = geometryWarning(tm);
		     if (!warn.empty()) out["warning"] = warn;
		     return out;
	     });

	tool("frame_cost",
	     "What one frame of an effect costs, in T-states. run_frames counts interrupts, which is "
	     "the wrong unit as soon as a render overruns one: this measures the work between two "
	     "frame boundaries - the CPU entering HALT by default - and keeps the time parked in HALT "
	     "separate, so work + idle is the wall clock and idle is exactly the headroom. Reports "
	     "every frame plus min/max/avg, how many interrupts a frame takes (the quantum), the "
	     "budget that many interrupts give, the headroom and the frame rate that follows. "
	     "Position the machine first (a breakpoint on the main loop, or run_frames); the partial "
	     "frame up to the first boundary is measured and reported separately, not averaged in.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "effect frames to measure, default 8"}}},
		     {"sync", {{"type", "string"}, {"description", "halt (default) | an address/label the frame ends at"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before measuring - the precalculation "
				     "otherwise lands in alignment_t"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "per-frame budget, default 20000000"}}}
	     }}},
	     [](const json& a) {
		     int frames = argNumOr(a, "frames", 8);
		     if (frames < 1) frames = 1;
		     if (frames > 10000) frames = 10000;
		     json skipped = skipUntil(a);
		     SyncSpec sync = argSync(a);
		     if (sync.byInterrupt)
			     throw std::runtime_error("sync=frame measures a hardware frame, which is "
						      "always t_states_per_frame; use halt or an address");
		     const long long budget = argNumOr(a, "max_instructions", 20000000);
		     Computer* c = g_mach.comp();
		     Timing tm = timing(c);
		     const long long nsPerT = c->nsPerTick > 0 ? c->nsPerTick : 1;

		     // land on a boundary first, so frame 1 is a whole frame. The
		     // profiler must not see this one: it is a partial frame, and
		     // counting it would skew a profile taken in the same pass.
		     xsp::FrameCost align = g_mach.frameCost(sync.pc, budget, false);
		     if (!align.complete)
			     throw std::runtime_error("no frame boundary (" + sync.text + ") within " +
						      std::to_string(budget) + " instructions - is the "
						      "machine running the effect?");

		     json list = json::array();
		     long long mn = -1, mx = 0, sum = 0;
		     for (int i = 0; i < frames; i++) {
			     xsp::FrameCost fc = g_mach.frameCost(sync.pc, budget);
			     if (!fc.complete) break;
			     long long work = fc.work_ns / nsPerT;
			     long long idle = fc.idle_ns / nsPerT;
			     if (mn < 0 || work < mn) mn = work;
			     if (work > mx) mx = work;
			     sum += work;
			     list.push_back({{"frame", i}, {"t_states", work}, {"idle_t", idle},
					     {"total_t", work + idle}, {"interrupts", fc.interrupts},
					     {"instructions", fc.instructions}});
		     }
		     if (list.empty())
			     throw std::runtime_error("no complete frame within the budget");

		     const int perInt = tm.tPerFrame > 0 ? tm.tPerFrame : 1;
		     const long long q = (mx + perInt - 1) / perInt;		// ceil
		     const long long budgetT = q * perInt;
		     json out{
			     {"sync", sync.text},
			     {"frames", (int)list.size()},
			     {"frame_list", list},
			     {"alignment_t", align.work_ns / nsPerT},
			     {"t_states_min", mn},
			     {"t_states_max", mx},
			     {"t_states_avg", (double)sum / (double)list.size()},
			     {"t_states_per_interrupt", perInt},
			     {"interrupts_per_frame", q},
			     {"budget_t", budgetT},
			     {"headroom_t", budgetT - mx},
			     {"headroom_percent", budgetT ? 100.0 * (budgetT - mx) / budgetT : 0.0},
			     // effect_fps is the effect's own rate; frame_timing's fps_real is
			     // the machine's. Same word, different quantities, so this one says
			     // which it is. `fps` stays as it was.
			     {"effect_fps", q ? tm.fps / (double)q : 0.0},
			     {"fps", q ? tm.fps / (double)q : 0.0}
		     };
		     if (!skipped.is_null()) out["skipped_to"] = skipped;
		     // the budget and the headroom are only as right as the frame length
		     std::string warn = geometryWarning(tm);
		     if (!warn.empty()) out["warning"] = warn;
		     return out;
	     });

	tool("profile",
	     "Instruction and T-state profiler. action=start begins counting (optionally over named "
	     "address ranges), read reports where the CPU spent its time - both per named range and as "
	     "a histogram of the busiest 256-byte pages - stop and reset do what they say. Counting "
	     "covers run/run_frames/step. Each range reports its share of the frame budget, how many "
	     "times control entered it (calls) and what one call cost (t_per_call) - which is the "
	     "number you optimise against. t_states and t_per_call are self time: what ran with the "
	     "PC inside the range, with a callee's time belonging to the callee. The *_inclusive "
	     "figures beside them add everything the range called out to, which is what \"what does "
	     "this routine cost me\" means for anything that is not a leaf. ranges:\"symbols\" splits by the loaded label table "
	     "instead, one region per label up to the next, which is the first thing to do on code "
	     "you do not know.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "start | stop | read | reset"}}},
		     {"ranges", {{"description",
				 "[{tag,start,end}] to measure separately. start and end are addresses "
				 "or label names and both are required; the region is [start, end), so "
				 "naming the next label as end measures exactly one routine. Or the "
				 "string \"symbols\" to split by every non-local label"}}},
		     {"from", {{"type", "string"}, {"description", "symbols mode: lowest address, default $4000"}}},
		     {"to", {{"type", "string"}, {"description", "symbols mode: highest address, default $FFFF"}}},
		     {"max_ranges", {{"type", "integer"}, {"description", "symbols mode: regions to report, default 24"}}},
		     {"top", {{"type", "integer"}, {"description", "how many hot pages to report, default 8"}}}
	     }}},
	     [](const json& a) {
		     auto& p = g_mach.profile();
		     std::string action = argStr(a, "action", "read");
		     static bool bySymbols = false;
		     // A start that failed leaves the previous profile in place, and its
		     // numbers look like a perfectly good measurement of the ranges you
		     // thought you had just asked for. Remember the failure so read can
		     // say so - this is the case that costs an afternoon.
		     static std::string lastStartError;
		     if (action == "start") {
			     try {
			     // Ranges go into a temporary and are committed only once every
			     // bound has resolved. A start that throws half way through must
			     // not leave the profiler holding a mangled set of ranges that a
			     // later read would then present as if they were measurements.
			     std::vector<xsp::Profile::Range> next;
			     bool nextBySymbols = false;
			     if (a.contains("ranges") && a["ranges"].is_string()) {
				     std::string mode = xsp::trim(a["ranges"].get<std::string>());
				     if (mode != "symbols")
					     throw std::runtime_error("ranges must be an array of "
								      "{tag,start,end} or the string \"symbols\"");
				     if (!g_mach.labels().size())
					     throw std::runtime_error("ranges=\"symbols\" needs a symbol "
								      "table - run load_labels first");
				     const int from = argAddrRange(a, "from", 0x4000);
				     const int to = argAddrRange(a, "to", 0xffff);
				     // one region per non-local label, up to the next one. Local
				     // labels (name.sub) would cut routines into fragments, and
				     // SMC labels declared as "name equ $+1" sit inside an
				     // instruction, so only whole names are used as boundaries.
				     std::vector<std::pair<int, std::string>> pts;
				     for (const auto& e : g_mach.labels().all()) {
					     if (e.name.find('.') != std::string::npos) continue;
					     if (e.address < from || e.address > to) continue;
					     pts.push_back({e.address, e.name});
				     }
				     if (pts.empty())
					     throw std::runtime_error("no non-local labels between " +
								      hex16(from) + " and " + hex16(to));
				     for (size_t i = 0; i < pts.size(); i++) {
					     xsp::Profile::Range range;
					     range.tag = pts[i].second;
					     range.start = pts[i].first;
					     range.end = (i + 1 < pts.size()) ? pts[i + 1].first : to + 1;
					     if (range.end > range.start) next.push_back(range);
				     }
				     nextBySymbols = true;
			     } else if (a.contains("ranges") && a["ranges"].is_array()) {
				     for (const auto& r : a["ranges"]) {
					     xsp::Profile::Range range;
					     range.tag = r.value("tag", std::string("range"));
					     // an unresolvable bound must fail here: a range left at
					     // 0..0 counts nothing and reads as "never executed"
					     if (!argAddr(r, "start", range.start) ||
						 !argAddr(r, "end", range.end))
						     throw std::runtime_error("range '" + range.tag +
							     "' needs both start and end");
					     if (range.end < range.start) std::swap(range.start, range.end);
					     next.push_back(range);
				     }
				     nextBySymbols = false;
			     } else {
				     next = p.ranges;		// start again over the same regions
				     nextBySymbols = bySymbols;
			     }
			     p.ranges = next;			// nothing above threw: commit
			     bySymbols = nextBySymbols;
			     p.reset();
			     p.on = true;
			     p.armed = true;
			     lastStartError.clear();
			     } catch (const std::exception& e) {
				     lastStartError = e.what();
				     throw;
			     }
		     } else if (action == "stop") {
			     p.on = false;
		     } else if (action == "reset") {
			     p.reset();
		     } else if (action != "read") {
			     throw std::runtime_error("action must be start, stop, read or reset");
		     }

		     int top = argNumOr(a, "top", 8);
		     std::vector<std::pair<long long, int>> pages;
		     for (int i = 0; i < 256; i++)
			     if (p.page[i]) pages.push_back({p.page[i], i});
		     std::sort(pages.begin(), pages.end(), std::greater<std::pair<long long, int>>());
		     json hot = json::array();
		     for (int i = 0; i < (int)pages.size() && i < top; i++) {
			     int page = pages[i].second;
			     json e{{"page", hex16(page << 8)}, {"instructions", pages[i].first}};
			     if (p.instructions)
				     e["percent"] = 100.0 * pages[i].first / p.instructions;
			     hot.push_back(e);
		     }
		     Computer* c = g_mach.comp();
		     Timing tm = timing(c);
		     const long long nsPerT = c->nsPerTick > 0 ? c->nsPerTick : 0;
		     const long long totalT = nsPerT ? p.ns / nsPerT : 0;

		     // symbols mode makes one region per label, most of them empty:
		     // report the ones that ran, dearest first
		     std::vector<const xsp::Profile::Range*> order;
		     for (const auto& r : p.ranges)
			     if (!bySymbols || r.instructions) order.push_back(&r);
		     if (bySymbols)
			     std::sort(order.begin(), order.end(),
				       [](const xsp::Profile::Range* x, const xsp::Profile::Range* y) {
					       return x->ns > y->ns;
				       });
		     const int maxRanges = argNumOr(a, "max_ranges", 24);

		     json ranges = json::array();
		     for (size_t i = 0; i < order.size(); i++) {
			     if (bySymbols && (int)i >= maxRanges) break;
			     const auto& r = *order[i];
			     json e{{"tag", r.tag}, {"start", r.start}, {"end", r.end},
				    {"start_hex", hex16(r.start)}, {"end_hex", hex16(r.end)},
				    {"last_hex", hex16(r.end - 1)},	// end is exclusive
				    {"instructions", r.instructions}, {"ns", r.ns},
				    {"calls", r.calls}, {"entries", r.entries}};
			     long long t = nsPerT ? r.ns / nsPerT : 0;
			     if (nsPerT) e["t_states"] = t;
			     if (r.calls) e["t_per_call"] = (double)t / (double)r.calls;
			     // self time is what ran in here; inclusive adds what it called
			     // out to, which is what "what does this routine cost me" means
			     // for anything that is not a leaf
			     if (nsPerT) {
				     long long ti = r.inclusiveNs / nsPerT;
				     e["t_states_inclusive"] = ti;
				     e["instructions_inclusive"] = r.inclusiveInstructions;
				     if (r.calls) e["t_per_call_inclusive"] = (double)ti / (double)r.calls;
			     }
			     // a region whose first address never executed is usually a data
			     // label that swallowed the code lying after it
			     if (r.instructions && !r.startHits) e["start_never_executed"] = true;
			     if (totalT) e["percent"] = 100.0 * t / totalT;
			     if (p.frames > 0) {
				     double perFrame = (double)t / p.frames;
				     e["t_states_per_frame"] = perFrame;
				     if (tm.tPerFrame > 0)
					     e["percent_of_frame"] = 100.0 * perFrame / tm.tPerFrame;
			     }
			     ranges.push_back(e);
		     }
		     json res{
			     {"armed", p.armed},
			     {"running", p.on},
			     {"instructions", p.instructions},
			     {"frames", p.frames},
			     {"ns", p.ns},
			     {"t_states", totalT},
			     {"t_states_per_frame", tm.tPerFrame},	// the budget a range is measured against
			     {"fps_real", tm.fps},
			     {"hot_pages", hot},
			     {"ranges", ranges}
		     };
		     // Zeros from a profiler that was never started look exactly like
		     // zeros from code that never ran, and reading them as the second is
		     // an expensive mistake. Say which it is.
		     if (!lastStartError.empty()) {
			     res["last_start_failed"] = lastStartError;
			     res["warning"] = "the last profile start FAILED (" + lastStartError +
					      "), so nothing here describes the ranges you asked for - "
					      "it is either the profile from before that call or nothing "
					      "at all. Fix the start and run it again.";
		     } else if (!p.armed)
			     res["warning"] = "the profiler has never been started, so every number "
					      "here is zero because nothing was measured - not because "
					      "nothing ran. Call profile {\"action\":\"start\"} first, and "
					      "check that it succeeded: one bad label name in ranges "
					      "aborts the whole start.";
		     else if (!p.instructions)
			     res["warning"] = "the profiler is armed but no instruction has been "
					      "executed since it started - run, run_frames, step or "
					      "frame_cost feed it.";
		     if (!p.ranges.empty()) {
			     res["ranges_measured"] = (int)p.ranges.size();
			     res["ranges_reported"] = (int)ranges.size();
			     json other{{"instructions", p.otherInstructions},
					{"t_states", nsPerT ? p.otherNs / nsPerT : 0}};
			     if (totalT)
				     other["percent"] = 100.0 * (nsPerT ? p.otherNs / nsPerT : 0) / totalT;
			     res["unattributed"] = other;	// time in no range at all
		     }
		     return res;
	     });

	tool("trace",
	     "Ring buffer of executed addresses - the backtrace for \"where did the CPU go?\". "
	     "action: enable, disable, clear, dump. dump disassembles the last `count` addresses.",
	     json{{"properties", {
		     {"action", {{"type", "string"}}},
		     {"count", {{"type", "integer"}, {"description", "entries to dump, default 32"}}},
		     {"size", {{"type", "integer"}, {"description", "ring size when enabling, default 4096"}}}
	     }}},
	     [](const json& a) {
		     auto& t = g_mach.trace();
		     std::string action = argStr(a, "action", "dump");
		     if (action == "enable") {
			     t.reset((size_t)argNumOr(a, "size", 4096));
			     t.on = true;
		     } else if (action == "disable") {
			     t.on = false;
		     } else if (action == "clear") {
			     t.reset(t.capacity);
		     } else if (action != "dump") {
			     throw std::runtime_error("action must be enable, disable, clear or dump");
		     }
		     json lines = json::array();
		     if (action == "dump") {
			     for (int pc : t.dump((size_t)argNumOr(a, "count", 32))) {
				     int len = 0;
				     xMnem mn;
				     json e{{"address", pc}, {"address_hex", hex16(pc)},
					    {"text", g_mach.disasm(pc, &len, &mn)}};
				     if (const std::string* lab = g_mach.labels().atAddress(pc))
					     e["label"] = *lab;
				     lines.push_back(e);
			     }
		     }
		     return json{{"running", t.on}, {"capacity", (int)t.capacity}, {"trace", lines}};
	     });

	tool("beam_log",
	     "Record where the beam was every time a given address executed, without stopping. This "
	     "is the tool for raster timing: a breakpoint answers \"where is the beam this once\", and "
	     "the question raster code actually raises is whether the answer is the same on every "
	     "frame. Watch the entry of an interrupt handler, or the OUT that flips a bank, run a few "
	     "dozen frames, and the spread in t_states_frame is the jitter - which is usually the "
	     "whole diagnosis. The summary reports it per address without reading a single event: "
	     "hits are numbered within their frame and compared position by position across frames, "
	     "because a strip loop hits its OUT dozens of times a frame and a plain min/max over "
	     "every hit only restates that the loop spans the frame. `jitter_shape` says whether "
	     "every position drifts by the same amount, which is a constant offset of the whole "
	     "pass, or by different amounts, which is drift accumulating inside it. "
	     "action: enable, disable, clear, dump.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "enable | disable | clear | dump (default)"}}},
		     {"addresses", {{"type", "array"}, {"description",
				    "addresses or labels to watch, when enabling"},
				    {"items", {{"type", "string"}}}}},
		     {"count", {{"type", "integer"}, {"description", "events to dump, default 64"}}},
		     {"summary", {{"type", "boolean"}, {"description",
				  "per-address jitter across frames, default true"}}},
		     {"by_position", {{"type", "boolean"}, {"description",
				      "add the per-position breakdown; off by default because a "
				      "48-strip loop makes 48 entries and jitter_shape already "
				      "says whether they agree"}}},
		     {"size", {{"type", "integer"}, {"description", "ring size when enabling, default 4096"}}}
	     }}},
	     [](const json& a) {
		     auto& rl = g_mach.beamLog();
		     const std::string action = argStr(a, "action", "dump");
		     if (action == "enable") {
			     if (!a.contains("addresses") || !a["addresses"].is_array() ||
				 a["addresses"].empty())
				     throw std::runtime_error("enable needs addresses[]");
			     rl.reset((size_t)argNumRange(a, "size", 4096, 1, 1 << 20));
			     rl.clearWatch();
			     for (const auto& item : a["addresses"]) {
				     int adr = 0;
				     json one = json::object();
				     one["a"] = item;
				     if (!argAddr(one, "a", adr))
					     throw std::runtime_error("bad address: " + item.dump());
				     if (adr < 0 || adr > 0xffff)
					     throw std::runtime_error("address outside $0000..$FFFF: " +
								      item.dump());
				     rl.addWatch(adr);
			     }
			     rl.on = true;
		     } else if (action == "disable") {
			     rl.on = false;
		     } else if (action == "clear") {
			     rl.reset(rl.capacity);
		     } else if (action != "dump") {
			     throw std::runtime_error("action must be enable, disable, clear or dump");
		     }

		     json watched = json::array();
		     for (int adr : rl.watched()) {
			     json w{{"address", adr}, {"address_hex", hex16(adr)}};
			     if (const std::string* lab = g_mach.labels().atAddress(adr))
				     w["label"] = *lab;
			     watched.push_back(w);
		     }
		     json res{{"running", rl.on}, {"capacity", (int)rl.capacity},
			      {"events_total", rl.total}, {"watching", watched}};

		     if (action == "dump") {
			     std::vector<xsp::BeamLog::Event> evs =
				     rl.dump((size_t)argNumRange(a, "count", 64, 1, 1 << 20));
			     json list = json::array();
			     for (const auto& e : evs)
				     list.push_back({{"address", e.pc}, {"address_hex", hex16(e.pc)},
						     {"frame_counter", e.frame},
						     {"t_states_frame", e.t},
						     {"line", e.line}, {"dot", e.dot}});
			     res["events"] = list;
			     if (argBool(a, "summary", true)) {
				     // Over the whole ring, not just the dumped tail: the spread
				     // is the answer here and it should not depend on `count`.
				     //
				     // Raster code runs the same address many times per frame -
				     // a multicolour strip loop hits its OUT once per band - so
				     // the min and max over every hit merely restate that the
				     // loop spans the frame. Jitter is the Nth hit of one frame
				     // against the Nth hit of the next, and that is what is
				     // measured here: hits are numbered within their frame and
				     // compared across frames position by position.
				     std::vector<xsp::BeamLog::Event> all = rl.dump(rl.capacity);
				     // The ring usually begins mid-frame, and that frame is
				     // missing its early hits, which would fake a huge spread on
				     // every position. Start at the first frame boundary we saw.
				     size_t begin = 0;
				     if (!all.empty()) {
					     const int f0 = all[0].frame;
					     while (begin < all.size() && all[begin].frame == f0) begin++;
				     }
				     struct Occ { int lo, hi; long long n; };
				     std::map<std::pair<int, int>, Occ> byOcc;	// (pc, nth in frame)
				     std::map<int, long long> hits;
				     std::map<int, std::map<int, int>> perFrame;	// pc -> frame -> count
				     std::map<int, int> nth;
				     int curFrame = begin < all.size() ? all[begin].frame : 0;
				     for (size_t i = begin; i < all.size(); i++) {
					     const auto& e = all[i];
					     if (e.frame != curFrame) { curFrame = e.frame; nth.clear(); }
					     const int k = nth[e.pc]++;
					     hits[e.pc]++;
					     perFrame[e.pc][e.frame]++;
					     auto key = std::make_pair(e.pc, k);
					     auto it = byOcc.find(key);
					     if (it == byOcc.end()) byOcc.emplace(key, Occ{e.t, e.t, 1});
					     else {
						     Occ& o = it->second;
						     if (e.t < o.lo) o.lo = e.t;
						     if (e.t > o.hi) o.hi = e.t;
						     o.n++;
					     }
				     }
				     const bool wantPos = argBool(a, "by_position", false);
				     json sum = json::array();
				     for (const auto& kv : hits) {
					     const int pc = kv.first;
					     int worst = 0, worstAt = -1, positions = 0, best = -1;
					     json byPos = json::array();
					     for (const auto& oc : byOcc) {
						     if (oc.first.first != pc) continue;
						     positions++;
						     const int spread = oc.second.hi - oc.second.lo;
						     if (spread > worst) { worst = spread; worstAt = oc.first.second; }
						     if (best < 0 || spread < best) best = spread;
						     if (wantPos && byPos.size() < 512)
							     byPos.push_back({{"nth_in_frame", oc.first.second},
									      {"frames", oc.second.n},
									      {"t_min", oc.second.lo},
									      {"t_max", oc.second.hi},
									      {"jitter_t", spread}});
					     }
					     int pfLo = -1, pfHi = -1;
					     for (const auto& f : perFrame[pc]) {
						     if (pfLo < 0 || f.second < pfLo) pfLo = f.second;
						     if (f.second > pfHi) pfHi = f.second;
					     }
					     // Whether the jitter is the same on every position or
					     // grows along them is the whole difference between a
					     // constant offset of the pass and drift accumulating
					     // inside it. That is two numbers, not one entry per
					     // position, and a 48-strip loop has 48 of those.
					     json s{{"address", pc}, {"address_hex", hex16(pc)},
						    {"hits", kv.second},
						    {"frames", (long long)perFrame[pc].size()},
						    {"hits_per_frame_min", pfLo},
						    {"hits_per_frame_max", pfHi},
						    {"jitter_t", worst},
						    {"jitter_t_least", best < 0 ? 0 : best},
						    {"jitter_worst_nth_in_frame", worstAt},
						    {"positions_per_frame", positions},
						    {"jitter_shape", positions < 2 ? "single"
								    : (best == worst ? "uniform" : "varies")}};
					     if (wantPos) s["by_position"] = byPos;
					     if (pfLo != pfHi)
						     s["warning"] = "the number of hits per frame is not "
								    "constant, so positions do not line up "
								    "between frames and jitter_t is unreliable";
					     if (const std::string* lab = g_mach.labels().atAddress(pc))
						     s["label"] = *lab;
					     sum.push_back(s);
				     }
				     res["summary"] = sum;
				     if (begin >= all.size() && !all.empty())
					     res["summary_note"] = "every event in the ring belongs to one "
								   "frame; run more frames, or raise `size`, "
								   "to compare frames against each other";
			     }
		     }
		     return res;
	     });

	// -------------------------------------------------- sound

	tool("ay_state",
	     "Decode the AY/YM sound chip: raw registers plus, per channel, the tone period as a "
	     "frequency in Hz, the volume, whether tone/noise are enabled in the mixer and whether the "
	     "envelope drives the volume. `audible` says whether any channel could currently be heard.",
	     json{{"properties", {{"chip", {{"type", "integer"}, {"description", "0=A (default), 1=B for Turbo Sound"}}}}}},
	     [](const json& a) {
		     auto st = xsp::audio::ayState(g_mach.comp(), argNumOr(a, "chip", 0));
		     if (!st.present) return json{{"present", false}};
		     json regs = json::array();
		     std::string hex;
		     char buf[8];
		     for (int i = 0; i < 16; i++) {
			     regs.push_back(st.reg[i]);
			     snprintf(buf, sizeof(buf), "%02X", st.reg[i]);
			     if (i) hex += ' ';
			     hex += buf;
		     }
		     json chans = json::array();
		     for (int i = 0; i < 3; i++) {
			     const auto& c = st.ch[i];
			     chans.push_back({
				     {"channel", std::string(1, c.name)},
				     {"period", c.period}, {"hz", c.hz},
				     {"volume", c.volume}, {"tone", c.tone},
				     {"noise", c.noise}, {"envelope", c.envelope},
				     {"level", c.level}
			     });
		     }
		     return json{
			     {"present", true}, {"chip", st.chip}, {"frq_mhz", st.frq_mhz},
			     {"registers", regs}, {"registers_hex", hex},
			     {"channels", chans},
			     {"noise", {{"period", st.noise_period}, {"hz", st.noise_hz}}},
			     {"envelope", {{"period", st.env_period}, {"hz", st.env_hz}, {"shape", st.env_shape}}},
			     {"audible", st.audible}
		     };
	     });

	tool("sound_state",
	     "Every sound source at once: beeper level, both AY chips of a Turbo Sound pair, General "
	     "Sound, SAA1099, Soundrive, and the machine's current mixed output level. Use it to see "
	     "what is even wired up before chasing silence.",
	     json{},
	     [](const json&) {
		     Computer* c = g_mach.comp();
		     json chips = json::array();
		     for (int i = 0; i < 2; i++) {
			     auto st = xsp::audio::ayState(c, i);
			     if (!st.present) continue;
			     chips.push_back({{"index", i}, {"chip", st.chip},
					      {"frq_mhz", st.frq_mhz}, {"audible", st.audible}});
		     }
		     sndVolume vol;
		     memset(&vol, 0, sizeof(vol));
		     vol.master = vol.beep = vol.tape = vol.ay = vol.gs = vol.sdrv = vol.saa = 100;
		     sndPair mix = {0, 0};
		     if (c->hw && c->hw->vol) mix = c->hw->vol(c, &vol);
		     sndPair ay = c->ts ? tsGetVolume(c->ts) : sndPair{0, 0};
		     sndPair gs = c->gs ? gsVolume(c->gs) : sndPair{0, 0};
		     return json{
			     {"beeper", {{"level", c->beep ? c->beep->lev : 0},
					 {"value", c->beep ? c->beep->val : 0}}},
			     {"ay_chips", chips},
			     {"ay_level", {{"left", ay.left}, {"right", ay.right}}},
			     {"general_sound", {{"level_left", gs.left}, {"level_right", gs.right}}},
			     {"saa1099", {{"enabled", c->saa && c->saa->enabled ? true : false}}},
			     {"tape", {{"playing", c->tape && c->tape->on ? true : false},
				       {"level", c->tape ? c->tape->volPlay : 0}}},
			     {"mixed_output", {{"left", mix.left}, {"right", mix.right}}}
		     };
	     });

	tool("audio_capture",
	     "Listen: run N frames while sampling the machine's mixed audio output (beeper + AY + tape). "
	     "Reports whether there is any sound at all, its peak and RMS level, a rough dominant "
	     "frequency from zero crossings, and how often the beeper toggled. With `path` it also "
	     "writes a .wav you can play. With `watch_ay` it logs every AY register write that "
	     "happened, with the frame, T-state and the PC that did it - that is your view of a music "
	     "driver at work.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "frames to listen for, default 50 (1 second)"}}},
		     {"rate", {{"type", "integer"}, {"description", "sample rate, default 44100"}}},
		     {"path", {{"type", "string"}, {"description", "write a .wav here"}}},
		     {"watch_ay", {{"type", "boolean"}, {"description", "log AY register writes, default false"}}},
		     {"max_writes", {{"type", "integer"}, {"description", "AY writes to return, default 32"}}}
	     }}},
	     [](const json& a) {
		     int frames = argNumOr(a, "frames", 50);
		     if (frames < 1) frames = 1;
		     if (frames > 3000) frames = 3000;
		     std::string path = argStr(a, "path");
		     bool watchAy = argBool(a, "watch_ay", false);

		     auto& cap = g_mach.audio();
		     cap.start(argNumOr(a, "rate", 44100), !path.empty(), watchAy);
		     json run = runToJson(g_mach.runFrames(frames));
		     cap.stop();

		     auto s = xsp::audio::summarize(cap);
		     json res{
			     {"frames", frames},
			     {"duration_s", s.duration_s},
			     {"samples", (long long)s.samples},
			     {"rate", s.rate},
			     {"silent", s.silent},
			     {"peak", s.peak},
			     {"rms", s.rms},
			     {"dominant_hz", s.dominant_hz},
			     {"beeper_toggles", s.beeper_toggles},
			     {"dominant_source", s.dominant_source},
			     {"source_rms", {{"beeper", s.beeper_rms}, {"ay", s.ay_rms},
					     {"general_sound", s.gs_rms}}},
			     {"raw_range", {{"min", s.raw_min}, {"max", s.raw_max}}},
			     {"run", run}
		     };
		     if (!path.empty()) {
			     std::string err;
			     if (!xsp::audio::writeWav(path, cap, err)) throw std::runtime_error(err);
			     res["wav"] = path;
		     }
		     if (watchAy) {
			     int limit = argNumOr(a, "max_writes", 32);
			     json writes = json::array();
			     int n = 0;
			     for (const auto& w : cap.writes) {
				     if (n++ >= limit) break;
				     writes.push_back({{"frame", w.frame}, {"t_state", w.t_state},
						       {"register", w.reg}, {"value", w.value},
						       {"pc", w.pc}, {"pc_hex", hex16(w.pc)}});
			     }
			     res["ay_writes_total"] = (int)cap.writes.size();
			     res["ay_writes"] = writes;
		     }
		     return res;
	     });

	tool("ay_writes",
	     "The AY register writes recorded by the last audio_capture that had watch_ay on, from "
	     "`offset`. Use it to page through a music driver's whole frame.",
	     json{{"properties", {
		     {"offset", {{"type", "integer"}}},
		     {"limit", {{"type", "integer"}, {"description", "default 64"}}}
	     }}},
	     [](const json& a) {
		     const auto& cap = g_mach.audio();
		     int offset = argNumRange(a, "offset", 0, 0, 0x7fffffff);
		     int limit = argNumRange(a, "limit", 64, 1, 100000);
		     json writes = json::array();
		     // end computed wide: offset near INT_MAX plus a limit overflows
		     const long long end = (long long)offset + limit;
		     for (int i = offset; i < (int)cap.writes.size() && i < end; i++) {
			     const auto& w = cap.writes[i];
			     writes.push_back({{"frame", w.frame}, {"t_state", w.t_state},
					       {"register", w.reg}, {"value", w.value},
					       {"pc", w.pc}, {"pc_hex", hex16(w.pc)}});
		     }
		     return json{{"total", (int)cap.writes.size()}, {"offset", offset}, {"writes", writes}};
	     });

	// -------------------------------------------------- keyboard

	tool("press_key",
	     "Hold a key for a few frames and release it. Names: a-z, 0-9, enter, space, caps, symbol, "
	     "delete, break, left/right/up/down, edit, capslock, graph, extend. The ROM needs the key "
	     "held for at least two frames to notice it.",
	     json{{"properties", {
		     {"key", {{"type", "string"}}},
		     {"frames", {{"type", "integer"}, {"description", "frames to hold, default 3"}}},
		     {"release_frames", {{"type", "integer"}, {"description", "frames after release, default 3"}}}
	     }}, {"required", json::array({"key"})}},
	     [](const json& a) {
		     xsp::keyboard::Key key;
		     std::string name = argStr(a, "key");
		     if (!xsp::keyboard::lookup(name, key))
			     throw std::runtime_error("unknown key '" + name + "'");
		     int hold = argNumRange(a, "frames", 3, 1, kMaxFrames);
		     int gap = argNumRange(a, "release_frames", 3, 0, kMaxFrames);
		     xsp::keyboard::press(g_mach.comp(), key);
		     g_mach.runFrames(hold);
		     xsp::keyboard::release(g_mach.comp(), key);
		     if (gap > 0) g_mach.runFrames(gap);
		     return json{{"key", name}, {"frames", hold + gap},
			     {"pc", cpu_get_pc(g_mach.comp()->cpu)},
			     {"pc_hex", hex16(cpu_get_pc(g_mach.comp()->cpu))}};
	     });

	tool("type_text",
	     "Type a string on the ZX keyboard, key by key, letting the machine run between presses. "
	     "Upper case and symbols get their shift automatically; \\n presses Enter. Remember the "
	     "48K ROM is in keyword-entry mode, so \"LOAD\" is one keypress (j), not four.",
	     json{{"properties", {
		     {"text", {{"type", "string"}}},
		     {"frames", {{"type", "integer"}, {"description", "frames per key, default 3"}}}
	     }}, {"required", json::array({"text"})}},
	     [](const json& a) {
		     std::string text = argStr(a, "text");
		     int hold = argNumRange(a, "frames", 3, 1, kMaxFrames);
		     int typed = 0;
		     std::string skipped;
		     for (char ch : text) {
			     xsp::keyboard::Key key;
			     if (!xsp::keyboard::forChar(ch, key)) { skipped += ch; continue; }
			     xsp::keyboard::press(g_mach.comp(), key);
			     g_mach.runFrames(hold);
			     xsp::keyboard::release(g_mach.comp(), key);
			     g_mach.runFrames(hold);
			     typed++;
		     }
		     return json{{"typed", typed}, {"skipped", skipped},
			     {"pc", cpu_get_pc(g_mach.comp()->cpu)},
			     {"pc_hex", hex16(cpu_get_pc(g_mach.comp()->cpu))}};
	     });

	tool("release_keys",
	     "Release every key - use it if a held key got stuck.",
	     json{},
	     [](const json&) {
		     xsp::keyboard::releaseAll(g_mach.comp());
		     return json{{"ok", true}};
	     });

	// -------------------------------------------------- files

	tool("load_file",
	     "Load a file into the machine by extension: .sna/.z80 snapshots, .tap/.tzx tapes, "
	     ".trd/.scl disks (drive A by default), .bin raw. Snapshots start running immediately.",
	     json{{"properties", {
		     {"path", {{"type", "string"}}},
		     {"drive", {{"type", "integer"}, {"description", "disk drive 0..3, default 0"}}}
	     }}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string path = argStr(a, "path");
		     if (path.empty()) throw std::runtime_error("path required");
		     std::string ext;
		     size_t dot = path.find_last_of('.');
		     if (dot != std::string::npos) ext = path.substr(dot + 1);
		     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
		     int drv = argNumOr(a, "drive", 0);
		     Computer* c = g_mach.comp();
		     int res;
		     if (ext == "sna") res = loadSNA(c, path.c_str(), drv);
		     else if (ext == "z80") res = loadZ80(c, path.c_str(), drv);
		     else if (ext == "tap") res = loadTAP(c, path.c_str(), drv);
		     else if (ext == "tzx") res = loadTZX(c, path.c_str(), drv);
		     else if (ext == "trd") res = loadTRD(c, path.c_str(), drv);
		     else if (ext == "scl") res = loadSCL(c, path.c_str(), drv);
		     else if (ext == "bin") res = loadBIN(c, path.c_str(), drv);
		     else throw std::runtime_error("unsupported extension '" + ext + "'");
		     if (res != ERR_OK) throw std::runtime_error("loader failed with code " + std::to_string(res));
		     return json{{"path", path}, {"format", ext}, {"ok", true},
			     {"pc", cpu_get_pc(c->cpu)}, {"pc_hex", hex16(cpu_get_pc(c->cpu))}};
	     });

	tool("tape",
	     "Tape deck: state (what is loaded, which block, is it playing), play, stop, rewind, next "
	     "block, eject, or blocks to list what is on the tape. A .tap/.tzx must be loaded first "
	     "with load_file - loading does not press play.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "state | play | stop | rewind | next | eject | blocks"}}},
		     {"block", {{"type", "integer"}, {"description", "block to rewind to, default 0"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = g_mach.comp();
		     Tape* t = c->tape;
		     std::string action = argStr(a, "action", "state");
		     // The core stores whatever block number it is handed and indexes
		     // blkData with it on the next play, so the range check has to
		     // happen here - upstream has none, and -1 is a segfault one call
		     // later rather than an error on the call that caused it.
		     if (action == "play") {
			     if (t->block < 0 || t->block >= t->blkCount)
				     throw std::runtime_error("tape is not at a playable block (" +
							      std::to_string(t->block) + " of " +
							      std::to_string(t->blkCount) + ")");
			     tapPlay(t);
		     }
		     else if (action == "stop") tapStop(t);
		     else if (action == "rewind") {
			     if (t->blkCount < 1) throw std::runtime_error("no tape loaded - see load_file");
			     tapRewind(t, argNumRange(a, "block", 0, 0, t->blkCount - 1));
		     }
		     else if (action == "next") tapNextBlock(t);
		     else if (action == "eject") tapEject(t);
		     else if (action != "state" && action != "blocks")
			     throw std::runtime_error("unknown tape action '" + action + "'");

		     json res{
			     {"path", t->path ? t->path : ""},
			     {"playing", t->on ? true : false},
			     {"recording", t->rec ? true : false},
			     {"block", t->block},
			     {"blocks", t->blkCount}
		     };
		     if (action == "blocks") {
			     std::vector<TapeBlockInfo> info((size_t)std::max(0, t->blkCount));
			     int n = info.empty() ? 0 : tapGetBlocksInfo(t, info.data(), (int)info.size());
			     json list = json::array();
			     for (int i = 0; i < n; i++)
				     list.push_back({{"index", i}, {"name", std::string(info[i].name)},
						     {"type", info[i].type}, {"size", info[i].size},
						     {"time_ms", info[i].time / 1000000}});
			     res["block_list"] = list;
		     }
		     return res;
	     });

	tool("disk",
	     "Floppy drives: state, insert a .trd/.scl image, eject, or save the current image back to "
	     "a file. Drive 0 is A.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "state | insert | eject | save"}}},
		     {"drive", {{"type", "integer"}, {"description", "0..3, default 0"}}},
		     {"path", {{"type", "string"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = g_mach.comp();
		     int drv = argNumOr(a, "drive", 0);
		     if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
		     Floppy* flp = c->dif->fdc->flop[drv];
		     std::string action = argStr(a, "action", "state");
		     std::string path = argStr(a, "path");

		     if (action == "insert") {
			     if (path.empty()) throw std::runtime_error("path required");
			     std::string ext;
			     size_t dot = path.find_last_of('.');
			     if (dot != std::string::npos) ext = path.substr(dot + 1);
			     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
			     int res = (ext == "scl") ? loadSCL(c, path.c_str(), drv)
						      : loadTRD(c, path.c_str(), drv);
			     if (res != ERR_OK)
				     throw std::runtime_error("loader failed with code " + std::to_string(res));
		     } else if (action == "eject") {
			     flp->insert = 0;
		     } else if (action == "save") {
			     if (path.empty()) throw std::runtime_error("path required");
			     int res = saveTRD(c, path.c_str(), drv);
			     if (res != ERR_OK)
				     throw std::runtime_error("save failed with code " + std::to_string(res));
		     } else if (action != "state") {
			     throw std::runtime_error("unknown disk action '" + action + "'");
		     }
		     // an inserted image the machine has no controller for reads as a working setup
		     // right up to the point the ROM waits for a drive that will never answer
		     return json{{"drive", drv}, {"inserted", flp->insert ? true : false},
			     {"path", flp->path ? flp->path : ""},
			     {"changed", flp->changed ? true : false},
			     {"interface", diskInterfaceName(c->dif->type)}};
	     });

	tool("disk_catalog",
	     "The TR-DOS catalogue of the disk in a drive: file names, extensions, start address, "
	     "length and where they sit on the disk.",
	     json{{"properties", {{"drive", {{"type", "integer"}}}}}},
	     [](const json& a) {
		     int drv = argNumOr(a, "drive", 0);
		     if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
		     Floppy* flp = g_mach.comp()->dif->fdc->flop[drv];
		     if (!flp->insert) throw std::runtime_error("no disk in drive " + std::to_string(drv));
		     std::vector<TRFile> cat(128);
		     int n = diskGetTRCatalog(flp, cat.data());
		     json list = json::array();
		     for (int i = 0; i < n; i++) {
			     const TRFile& f = cat[i];
			     std::string name((const char*)f.name, 8);
			     while (!name.empty() && name.back() == ' ') name.pop_back();
			     list.push_back({
				     {"name", name},
				     {"ext", std::string(1, (char)f.ext)},
				     {"start", (f.hst << 8) | f.lst},
				     {"start_hex", hex16((f.hst << 8) | f.lst)},
				     {"length", (f.hlen << 8) | f.llen},
				     {"sectors", f.slen},
				     {"track", f.trk},
				     {"sector", f.sec}
			     });
		     }
		     return json{{"drive", drv}, {"files", (int)n}, {"catalog", list}};
	     });

	tool("save_snapshot",
	     "Save the current machine state to a .sna file. The standard way to skip a long "
	     "precalculation: run once to the main loop, save here, and load that snapshot for "
	     "every later measurement instead of paying for the precalculation each time.",
	     json{{"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string path = argStr(a, "path");
		     int res = saveSNA(g_mach.comp(), path.c_str(), 0);
		     if (res != ERR_OK) throw std::runtime_error("save failed with code " + std::to_string(res));
		     return json{{"path", path}, {"ok", true}};
	     });
}

// ------------------------------------------------------------ JSON-RPC

static json okResult(const json& payload) {
	return json{
		{"content", json::array({json{{"type", "text"}, {"text", payload.dump(1, '\t')}}})},
		{"isError", false}
	};
}

static json errResult(const std::string& msg) {
	return json{
		{"content", json::array({json{{"type", "text"}, {"text", msg}}})},
		{"isError", true}
	};
}

static void reply(const json& id, const json& result) {
	json msg{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
	std::cout << msg.dump() << std::endl;
}

static void replyError(const json& id, int code, const std::string& message) {
	json msg{{"jsonrpc", "2.0"}, {"id", id},
		 {"error", {{"code", code}, {"message", message}}}};
	std::cout << msg.dump() << std::endl;
}

static void handle(const json& req) {
	const std::string method = req.value("method", "");
	const bool isNotification = !req.contains("id");
	const json id = req.contains("id") ? req["id"] : json(nullptr);

	if (method == "initialize") {
		std::string proto = "2024-11-05";
		if (req.contains("params") && req["params"].contains("protocolVersion"))
			proto = req["params"]["protocolVersion"].get<std::string>();
		reply(id, json{
			{"protocolVersion", proto},
			{"capabilities", {{"tools", json::object()}}},
			{"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
			{"instructions", kInstructions}
		});
		return;
	}
	if (method == "notifications/initialized" || method == "notifications/cancelled") return;
	if (method == "ping") { reply(id, json::object()); return; }

	if (method == "tools/list") {
		json list = json::array();
		for (const auto& t : g_tools)
			list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
		reply(id, json{{"tools", list}});
		return;
	}

	if (method == "tools/call") {
		const json params = req.value("params", json::object());
		const std::string name = params.value("name", "");
		const json args = params.contains("arguments") && params["arguments"].is_object()
			? params["arguments"] : json::object();
		for (const auto& t : g_tools) {
			if (t.name != name) continue;
			try {
				reply(id, okResult(t.fn(args)));
			} catch (const std::exception& e) {
				reply(id, errResult(std::string("error in ") + name + ": " + e.what()));
			}
			return;
		}
		reply(id, errResult("unknown tool '" + name + "'"));
		return;
	}

	if (!isNotification) replyError(id, -32601, "unknown method '" + method + "'");
}

int main(int argc, char** argv) {
	std::string configDir;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--version") {
			printf("%s %s\n", kServerName, kServerVersion);
			printf("Xpeccy %s", kXpeccyVersion);
			if (strcmp(kXpeccyVersion, kXpeccyPinned) != 0)
				printf(" (pinned: %s - this build does not match it)", kXpeccyPinned);
			printf("\n");
			return 0;
		}
		if (a == "--help") {
			printf("usage: %s [--config <xpeccy config dir>]\n", argv[0]);
			return 0;
		}
		if (a == "--config") {
			// a --config nobody can read is an error, not a quiet fall back to
			// the defaults and a machine the caller did not ask for
			if (i + 1 >= argc) {
				fprintf(stderr, "xspeccy-mcp: --config needs a directory\n");
				return 1;
			}
			configDir = argv[++i];
		}
		else if (a.compare(0, 9, "--config=") == 0) configDir = a.substr(9);
	}

	xsp::platform::setBinaryStdio();	// Windows would otherwise rewrite \n

	std::string err;
	if (!g_mach.init(err, configDir)) {
		fprintf(stderr, "xspeccy-mcp: %s\n", err.c_str());
		return 1;
	}
	fprintf(stderr, "xspeccy-mcp: %s\n", g_mach.env().note.c_str());
	g_mach.runFrames(100);			// reach the prompt before the first request

	registerTools();
	fprintf(stderr, "xspeccy-mcp: %s, Xpeccy %s%s\n", kServerVersion, kXpeccyVersion,
		strcmp(kXpeccyVersion, kXpeccyPinned) == 0 ? "" : " (NOT the pinned release)");
	fprintf(stderr, "xspeccy-mcp: %zu tools ready\n", g_tools.size());

	std::ios::sync_with_stdio(false);
	std::string line;
	while (std::getline(std::cin, line)) {
		line = xsp::trim(line);		// also drops the \r of a CRLF client
		if (line.empty()) continue;
		json req;
		try {
			req = json::parse(line);
		} catch (const std::exception& e) {
			replyError(nullptr, -32700, std::string("parse error: ") + e.what());
			continue;
		}
		try {
			handle(req);
		} catch (const std::exception& e) {
			fprintf(stderr, "xspeccy-mcp: %s\n", e.what());
		}
	}
	return 0;
}
