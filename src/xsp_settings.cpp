#include "xsp_settings.h"

#include "xsp_blend.h"
#include "xsp_machine.h"
#include "xsp_platform.h"
#include "xsp_video.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

extern "C" {
#include "fdc.h"
}

namespace xsp {
namespace settings {

namespace {

// ---- enum vocabularies ------------------------------------------------
// Names, not numbers, in the file and in the tool: `contPattern = b` survives
// being read by a person, `contPattern = 2` does not.

const char* kContNames[] = {"none", "a", "b", nullptr};
const char* kResetNames[] = {"default", "48", "128", "dos", "shadow", nullptr};
// Indexed by the core's own numbering, which starts at SND_NONE = 0. Naming
// these by position rather than by value put "ay" on SND_NONE and reported an
// AY as a YM, which is the kind of mistake a table like this exists to make
// impossible - so the table has to line up with the enum, not with the order
// the names read nicely in.
const char* kChipNames[] = {"none", "ay", "ym", "ym2203", nullptr};
const char* kStereoNames[] = {"mono", "abc", "acb", nullptr};

// ---- reaching into the machine ----------------------------------------
//
// Every accessor tolerates a half-built machine: the settings file is read
// before anything has run, and a missing sound chip must read as its default
// rather than crash.

ulaPlus* ula(Machine& m) {
	Computer* c = m.comp();
	return (c && c->vid) ? c->vid->ula : nullptr;
}

aymChip* chip(Machine& m) {
	Computer* c = m.comp();
	return (c && c->ts) ? c->ts->chipA : nullptr;
}

double getContPattern(Machine& m) { ulaPlus* u = ula(m); return u ? u->conttype : CONT_PATA; }
void setContPattern(Machine& m, double v) { if (ulaPlus* u = ula(m)) u->conttype = (int)v; }

double getEarly(Machine& m) { ulaPlus* u = ula(m); return (u && u->early) ? 1 : 0; }
void setEarly(Machine& m, double v) { if (ulaPlus* u = ula(m)) u->early = v != 0; }

double getUlaPlus(Machine& m) { ulaPlus* u = ula(m); return (u && u->enabled) ? 1 : 0; }
void setUlaPlus(Machine& m, double v) { if (ulaPlus* u = ula(m)) u->enabled = v != 0; }

double getBrdStep(Machine& m) {
	Computer* c = m.comp();
	return (c && c->vid) ? c->vid->brdstep : 1;
}
void setBrdStep(Machine& m, double v) {
	Computer* c = m.comp();
	if (c && c->vid) c->vid->brdstep = (int)v;
}

double getContMem(Machine& m) { Computer* c = m.comp(); return (c && c->flgCNTM) ? 1 : 0; }
void setContMem(Machine& m, double v) { if (Computer* c = m.comp()) c->flgCNTM = v != 0; }

double getContIo(Machine& m) { Computer* c = m.comp(); return (c && c->flgCNTI) ? 1 : 0; }
void setContIo(Machine& m, double v) { if (Computer* c = m.comp()) c->flgCNTI = v != 0; }

double getScrWait(Machine& m) { Computer* c = m.comp(); return (c && c->flgEM1) ? 1 : 0; }
void setScrWait(Machine& m, double v) { if (Computer* c = m.comp()) c->flgEM1 = v != 0; }

double getDdPal(Machine& m) { Computer* c = m.comp(); return (c && c->flgDDP) ? 1 : 0; }
void setDdPal(Machine& m, double v) { if (Computer* c = m.comp()) c->flgDDP = v != 0; }

double getTurbo(Machine& m) { Computer* c = m.comp(); return c ? c->frqMul : 1.0; }
void setTurbo(Machine& m, double v) { if (Computer* c = m.comp()) compSetTurbo(c, v); }

double getResBank(Machine& m) { Computer* c = m.comp(); return c ? c->resbank : RES_48; }
void setResBank(Machine& m, double v) { if (Computer* c = m.comp()) c->resbank = (int)v; }

double getFdcTurbo(Machine&) { return fdcFlag ? 1 : 0; }
void setFdcTurbo(Machine&, double v) { fdcFlag = (v != 0) ? 1 : 0; }

double getChipType(Machine& m) { aymChip* a = chip(m); return a ? a->type : SND_AY; }
void setChipType(Machine& m, double v) { if (aymChip* a = chip(m)) a->type = (int)v; }

double getStereo(Machine& m) { aymChip* a = chip(m); return a ? a->stereo : AY_MONO; }
void setStereo(Machine& m, double v) { if (aymChip* a = chip(m)) a->stereo = (int)v; }

double getChipFrq(Machine& m) { aymChip* a = chip(m); return a ? a->frq : 1.7744; }
void setChipFrq(Machine& m, double v) { if (aymChip* a = chip(m)) a->frq = v; }

// video: the same storage video_config uses, so the two tools cannot disagree
double getBlend(Machine&) { return blend::defaults().frames; }
void setBlend(Machine&, double v) { blend::defaults().frames = (int)v; }

double getGamma(Machine&) { return blend::defaults().gamma; }
void setGamma(Machine&, double v) { blend::defaults().gamma = v; }

double getHistory(Machine&) { return blend::defaults().history; }
void setHistory(Machine&, double v) {
	blend::defaults().history = (int)v;
	blend::setDepth((int)v);
}

double getGrey(Machine&) { return greyScale ? 1 : 0; }
void setGrey(Machine&, double v) { greyScale = (v != 0) ? 1 : 0; }

double getBorder(Machine& m) { return m.borderSize() * 100.0; }
void setBorder(Machine& m, double v) { m.setBorderSize(v / 100.0); }

// ---- the table --------------------------------------------------------

const std::vector<Setting> g_table = {
	// Timing. These decide when code runs relative to the beam, which is what
	// multicolour is made of, so each one invalidates measurements taken before it.
	{"timing.contPattern", Type::Enum, kContNames, 0, 0, CONT_PATA, Scope::Live,
	 TimingBaseline, "contPattern",
	 "which RAM banks the ULA steals cycles from; nothing on a Pentagon, everything on a 48K/128K",
	 getContPattern, setContPattern},
	{"timing.earlyTiming", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 TimingBaseline, "earlyTiming",
	 "shifts the contention window 2 T earlier", getEarly, setEarly},
	{"timing.borderStep", Type::Int, nullptr, 1, 8, 1, Scope::Live,
	 TimingBaseline, "4t-border",
	 "T-states between border updates; 1 is the finest a border effect can be",
	 getBrdStep, setBrdStep},
	{"timing.contendedMemory", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 TimingBaseline, "contmem",
	 "memory contention on or off wholesale", getContMem, setContMem},
	{"timing.contendedIo", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 TimingBaseline, "contio", "I/O contention", getContIo, setContIo},
	{"timing.screenPortWait", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 TimingBaseline, "scrp.wait", "the screen-port wait state (Scorpion)",
	 getScrWait, setScrWait},
	{"timing.cpuMultiplier", Type::Double, nullptr, 0.1, 8.0, 1.0, Scope::Live,
	 TimingBaseline, "frq.mul",
	 "turbo multiplier on the CPU clock; 1.0 is the real machine",
	 getTurbo, setTurbo},

	// Boot and media.
	{"boot.resetBank", Type::Enum, kResetNames, 0, 0, RES_48, Scope::NeedsReset,
	 None, "[ROMSET] reset",
	 "which ROM page a reset lands in. `dos` only reaches TR-DOS if the romset actually "
	 "carries a TR-DOS ROM in that page - the stock ZX48 romset does not, and a reset into "
	 "it lands on a blank screen rather than an error",
	 getResBank, setResBank},
	{"disk.fdcTurbo", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 None, "fdcturbo",
	 "floppy operations finish instantly instead of at drive speed, which changes how long a load takes in emulated time",
	 getFdcTurbo, setFdcTurbo},

	// Picture. Anything that changes how a frame is drawn drops the blend
	// history: averaging frames drawn two different ways is meaningless.
	{"video.blend", Type::Int, nullptr, 1, blend::kMaxHistory, 1, Scope::Live,
	 None, nullptr,
	 "frames averaged into every capture; 1 is the raw frame, 2 is a gigascreen",
	 getBlend, setBlend},
	{"video.gamma", Type::Double, nullptr, 1.0, 3.0, 2.2, Scope::Live,
	 None, "noflick.gamma",
	 "transfer used to blend in linear light; 1 averages the bytes instead",
	 getGamma, setGamma},
	{"video.frameHistory", Type::Int, nullptr, 0, blend::kMaxHistory, 4, Scope::Live,
	 DropsHistory, nullptr,
	 "completed frames kept for blending; 0 switches blending off entirely",
	 getHistory, setHistory},
	{"video.greyscale", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 DropsHistory, "greyscale", "draw through the grey palette", getGrey, setGrey},
	{"video.borderSize", Type::Int, nullptr, 0, 100, 50, Scope::Live,
	 DropsHistory, "bordersize",
	 "percentage of the border the frame carries; changes the size of every picture",
	 getBorder, setBorder},
	{"video.ulaPlus", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 DropsHistory, "ULAplus", "64-colour ULAplus mode", getUlaPlus, setUlaPlus},
	{"video.ddPalette", Type::Bool, nullptr, 0, 1, 0, Scope::Live,
	 DropsHistory, "DDpal", "ATM2/PentEvo port decoding", getDdPal, setDdPal},

	// Sound. Nothing here was configurable before: the machine got whatever
	// compCreate() built.
	{"sound.chipType", Type::Enum, kChipNames, 0, 0, SND_AY, Scope::Live,
	 None, "chip1",
	 "AY-3-8910 or YM2149; different volume curves, so different output for the same registers",
	 getChipType, setChipType},
	{"sound.chipStereo", Type::Enum, kStereoNames, 0, 0, AY_MONO, Scope::Live,
	 None, "chip1.stereo", "channel panning", getStereo, setStereo},
	{"sound.chipFrequency", Type::Double, nullptr, 0.5, 8.0, 1.7744, Scope::Live,
	 None, "chip1.frq",
	 "AY clock in MHz; every note the chip plays scales with it",
	 getChipFrq, setChipFrq},
};

std::map<std::string, Source> g_source;

std::string trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char)s[a])) a++;
	while (b > a && isspace((unsigned char)s[b - 1])) b--;
	return s.substr(a, b - a);
}

std::string lower(std::string s) {
	for (char& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

} // namespace

const std::vector<Setting>& table() { return g_table; }

const Setting* find(const std::string& name) {
	const std::string want = lower(name);
	for (const Setting& s : g_table)
		if (lower(s.name) == want) return &s;
	return nullptr;
}

Source sourceOf(const std::string& name) {
	auto it = g_source.find(lower(name));
	return it == g_source.end() ? Source::Default : it->second;
}

void markSource(const std::string& name, Source src) { g_source[lower(name)] = src; }

const char* sourceName(Source s) {
	switch (s) {
	case Source::XpeccyProfile: return "xpeccy profile";
	case Source::File: return "settings file";
	case Source::Session: return "this session";
	default: return "core default";
	}
}

bool parseValue(const Setting& s, const std::string& text, double& out, std::string& err) {
	const std::string t = lower(trim(text));
	if (t.empty()) { err = "empty value"; return false; }

	if (s.type == Type::Enum) {
		for (int i = 0; s.enumNames[i]; i++)
			if (t == s.enumNames[i]) { out = i; return true; }
		std::string list;
		for (int i = 0; s.enumNames[i]; i++)
			list += (i ? ", " : "") + std::string(s.enumNames[i]);
		err = "'" + trim(text) + "' is not one of: " + list;
		return false;
	}
	if (s.type == Type::Bool) {
		if (t == "1" || t == "yes" || t == "true" || t == "on") { out = 1; return true; }
		if (t == "0" || t == "no" || t == "false" || t == "off") { out = 0; return true; }
		err = "'" + trim(text) + "' is not a yes/no value";
		return false;
	}
	char* end = nullptr;
	const double v = strtod(t.c_str(), &end);
	if (!end || *end) { err = "'" + trim(text) + "' is not a number"; return false; }
	if (s.type == Type::Int && v != std::floor(v)) {
		err = "'" + trim(text) + "' is not a whole number";
		return false;
	}
	out = v;
	return true;
}

std::string formatValue(const Setting& s, double v) {
	if (s.type == Type::Enum) {
		const int i = (int)v;
		for (int k = 0; s.enumNames[k]; k++)
			if (k == i) return s.enumNames[k];
		return std::to_string(i);
	}
	if (s.type == Type::Bool) return v != 0 ? "yes" : "no";
	if (s.type == Type::Int) return std::to_string((long long)v);
	char buf[64];
	snprintf(buf, sizeof(buf), "%g", v);
	return buf;
}

bool apply(Machine& m, const Setting& s, double value, std::string& err) {
	if (s.type == Type::Enum) {
		int n = 0;
		while (s.enumNames[n]) n++;
		if (value < 0 || value >= n) {
			err = std::string(s.name) + ": no such option";
			return false;
		}
	} else if (value < s.lo || value > s.hi) {
		// refused, never clamped: a silently corrected setting is a setting
		// that does not do what the file says it does
		err = std::string(s.name) + ": " + formatValue(s, value) + " is outside " +
		      formatValue(s, s.lo) + ".." + formatValue(s, s.hi);
		return false;
	}
	s.set(m, value);
	if (s.flags & DropsHistory) video::historyClear();
	return true;
}

// ---- the file ---------------------------------------------------------

namespace {

bool readOne(Machine& m, const std::string& path, LoadReport& rep) {
	std::ifstream in(path.c_str());
	if (!in) return false;
	rep.files.push_back(path);
	std::string line, section;
	int lineNo = 0;
	while (std::getline(in, line)) {
		lineNo++;
		const size_t hash = line.find('#');
		if (hash != std::string::npos) line = line.substr(0, hash);
		line = trim(line);
		if (line.empty()) continue;
		if (line.front() == '[' && line.back() == ']') {
			section = trim(line.substr(1, line.size() - 2));
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			rep.problems.push_back(path + ":" + std::to_string(lineNo) +
					       ": not KEY = VALUE");
			continue;
		}
		std::string key = trim(line.substr(0, eq));
		const std::string val = trim(line.substr(eq + 1));
		if (!section.empty() && key.find('.') == std::string::npos)
			key = section + "." + key;
		const Setting* s = find(key);
		if (!s) {
			rep.problems.push_back(path + ":" + std::to_string(lineNo) +
					       ": unknown setting '" + key + "'");
			continue;
		}
		double v = 0;
		std::string err;
		if (!parseValue(*s, val, v, err) || !apply(m, *s, v, err)) {
			rep.problems.push_back(path + ":" + std::to_string(lineNo) + ": " + err);
			continue;
		}
		markSource(s->name, Source::File);
		rep.applied.push_back(std::string(s->name) + " = " + formatValue(*s, v));
	}
	return true;
}

} // namespace

std::string defaultSavePath(const std::string& xpeccyConfigDir) {
	if (!xpeccyConfigDir.empty())
		return platform::join(xpeccyConfigDir, "xspeccy-mcp.conf");
	return "xspeccy-mcp.conf";
}

LoadReport loadFiles(Machine& m, const std::string& xpeccyConfigDir,
		     const std::string& explicitPath) {
	LoadReport rep;
	// Later wins, so the project's own file overrides the one beside the
	// emulator, and --settings overrides both.
	if (!xpeccyConfigDir.empty())
		readOne(m, platform::join(xpeccyConfigDir, "xspeccy-mcp.conf"), rep);
	readOne(m, "xspeccy-mcp.conf", rep);
	if (!explicitPath.empty() && !readOne(m, explicitPath, rep))
		rep.problems.push_back(explicitPath + ": cannot be read");
	return rep;
}

bool save(const std::string& path, const std::string& name, const std::string& value,
	  std::string& usedPath, std::string& err) {
	usedPath = path;
	// Rewrite in place: everything a person put in the file - comments,
	// order, settings this build does not know - has to survive being edited
	// by a tool, or nobody will keep anything in it.
	std::vector<std::string> lines;
	{
		std::ifstream in(usedPath.c_str());
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(line);
		}
	}
	const std::string want = lower(name);
	bool replaced = false;
	std::string section;
	for (std::string& line : lines) {
		std::string bare = line;
		const size_t hash = bare.find('#');
		if (hash != std::string::npos) bare = bare.substr(0, hash);
		bare = trim(bare);
		if (bare.size() > 1 && bare.front() == '[' && bare.back() == ']') {
			section = trim(bare.substr(1, bare.size() - 2));
			continue;
		}
		const size_t eq = bare.find('=');
		if (eq == std::string::npos) continue;
		std::string key = trim(bare.substr(0, eq));
		if (!section.empty() && key.find('.') == std::string::npos)
			key = section + "." + key;
		if (lower(key) != want) continue;
		const std::string indent(line.begin(),
					 line.begin() + (line.find_first_not_of(" \t") == std::string::npos
							 ? 0 : line.find_first_not_of(" \t")));
		std::string shortKey = trim(bare.substr(0, eq));
		line = indent + shortKey + " = " + value;
		replaced = true;
		break;
	}
	if (!replaced) {
		if (!lines.empty() && !lines.back().empty()) lines.push_back("");
		lines.push_back(name + " = " + value);
	}
	std::ofstream out(usedPath.c_str(), std::ios::binary | std::ios::trunc);
	if (!out) { err = "cannot write " + usedPath; return false; }
	for (const std::string& line : lines) out << line << "\n";
	return true;
}

} // namespace settings
} // namespace xsp
