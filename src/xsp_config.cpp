#include "xsp_config.h"
#include "xsp_platform.h"
#include "xsp_rom_builtin.h"	// generated: defines XSP_BUILTIN_ROM and kBuiltinRom
#include "xsp_builtin.h"	// the core's own definitions, layouts and roms

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

extern "C" {
#include "spectrum.h"		// RES_*, FBUS_*, EAR_*
#include "fdc.h"		// DIF_*
#include "hdd.h"		// IDE_*
#include "hardware/hardware.h"	// tabHwItem for the machine list order
#include "input/input.h"	// KBD_XT/AT/PS2
#include "video/video.h"	// VID_BRD_*
#include "sound/ayym.h"		// SND_*, AY_*
#include "sound/soundrive.h"	// SDRV_*
}

// defined in hardware/hardware.c; the header exposes only the item type
extern tabHwItem tabHwPtr[];

namespace xsp {

// The values a definition starts from before its file says anything -
// xcore/machines.cpp:mac_defaults(). A key the file does not carry keeps
// these, not whatever the previous machine had.
MachineDef::MachineDef()
	: cpu("Z80"), memoryKb(128), cpuMhz(3.5), turboSteps("1"),
	  ramNoise(0), resbank(RES_128), earback(EAR_ISSUE3), earRC(0),
	  contio(0), contmem(0), scrpwait(0), contPattern(0), early(0), brd4t(0),
	  snow(0), snowx(0), fbus(FBUS_NONE), brdmin(VID_BRD_NONE), ulaplus(0),
	  ddpal(0), disk(DIF_NONE), ide(IDE_NONE), drives(4),
	  psgCount(1), psgType(SND_AY), psgStereo(AY_MONO), psgFrq(0),
	  soundrive(SDRV_NONE), gs(0), saa(0),
	  joy(1), mouse(0), mouseWheel(0), scantab(0), romBanks(4) {}

std::string homeDir() {
	return platform::homeDir();
}

std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
	std::vector<std::string> res;
	size_t pos = 0, next;
	while ((next = s.find(sep, pos)) != std::string::npos) {
		res.push_back(s.substr(pos, next - pos));
		pos = next + 1;
	}
	res.push_back(s.substr(pos));
	return res;
}

static bool isDir(const std::string& p) { return platform::isDir(p); }
static bool isFile(const std::string& p) { return platform::isFile(p); }

// Walks an ini-ish xpeccy config: "[SECTION]" headers and "key = value" lines.
// The callback gets (section, key, value); repeated keys are all delivered.
template <typename F>
static bool parseIni(std::istream& f, F&& cb) {
	std::string line, section;
	while (std::getline(f, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		if (line.front() == '[' && line.back() == ']') {
			section = line.substr(1, line.size() - 2);
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		cb(section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
	}
	return true;
}

// ...from a file, which is false when it does not open. The stream form is for
// the copies compiled into the binary (xsp_builtin.h).
template <typename F>
static bool parseIni(const std::string& path, F&& cb) {
	std::ifstream f(path);
	if (!f.is_open()) return false;
	return parseIni(static_cast<std::istream&>(f), cb);
}

// Machine definition files go through upstream's reader instead
// (xcore/machines.cpp:mac_read): a '#' or ';' ANYWHERE starts a comment -
// "contmem = yes # always" is yes, and no value may carry either character -
// and section names fold to lower case, so [Machine] and [machine] are one.
// An empty value is kept, which is how a child says "rom0 = nothing here".
template <typename F>
static bool parseIniMachine(std::istream& f, F&& cb) {
	std::string line, section;
	bool first = true;
	while (std::getline(f, line)) {
		// A UTF-8 byte order mark, which an editor on Windows likes to put in
		// front of a file it saved: upstream reads through QTextStream, which
		// drops it, and left in it turns "[machine]" into a line that is not a
		// section header - every key under it would land in no section at all.
		if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
		first = false;
		size_t cut = line.find_first_of("#;");
		if (cut != std::string::npos) line.erase(cut);
		line = trim(line);
		if (line.empty()) continue;
		if (line.front() == '[') {
			size_t close = line.find(']');
			if (close != std::string::npos) {
				section = trim(line.substr(1, close - 1));
				for (auto& ch : section) ch = (char)tolower((unsigned char)ch);
			}
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		cb(section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
	}
	return true;
}

template <typename F>
static bool parseIniMachine(const std::string& path, F&& cb) {
	std::ifstream f(path);
	if (!f.is_open()) return false;
	return parseIniMachine(static_cast<std::istream&>(f), cb);
}

// "Pentagon:448:320:72:48:64:32:64:0:0:256:192"  (xcore/config.cpp:409-425)
static bool parseLayout(const std::string& val, Layout& lay) {
	auto v = split(val, ':');
	if (v.size() < 9) return false;
	lay.name     = v[0];
	lay.full_x   = atoi(v[1].c_str());
	lay.full_y   = atoi(v[2].c_str());
	lay.bord_x   = atoi(v[3].c_str());
	lay.bord_y   = atoi(v[4].c_str());
	lay.blank_x  = atoi(v[5].c_str());
	lay.blank_y  = atoi(v[6].c_str());
	lay.int_size = atoi(v[7].c_str());
	lay.int_y    = atoi(v[8].c_str());
	lay.int_x    = (v.size() > 9)  ? atoi(v[9].c_str())  : 0;
	lay.scr_x    = (v.size() > 10) ? atoi(v[10].c_str()) : 256;
	lay.scr_y    = (v.size() > 11) ? atoi(v[11].c_str()) : 192;
	if (lay.full_x > 512) lay.full_x = 512;
	if (lay.full_y > 512) lay.full_y = 512;
	return true;
}

// xpeccy writes doubles with the locale decimal point, so "1,774400" happens
static double parseNum(const std::string& s) {
	std::string t = s;
	for (char& c : t) if (c == ',') c = '.';
	return atof(t.c_str());
}

void Env::load(const std::string& forcedDir) {
	// where the emulator keeps its configuration
	std::vector<std::string> candidates;
	if (!forcedDir.empty()) candidates.push_back(forcedDir);
	for (const auto& c : platform::xpeccyConfigCandidates()) candidates.push_back(c);

	confDir = candidates.empty() ? "" : candidates.front();
	for (const auto& c : candidates) {
		if (platform::isDir(c)) { confDir = c; break; }
	}
	romDir = platform::join(confDir, "roms");
	prfDir = platform::join(confDir, "profiles");

	layouts.push_back(Layout{});		// built-in "default" (xcore/config.cpp:104)

	configFound = isDir(confDir);

	std::string cur;
	if (configFound) {
		Romset* rs = nullptr;
		parseIni(platform::join(confDir, "config.conf"),
			[&](const std::string& sect, const std::string& key, const std::string& val) {
				if (sect == "PROFILES" && key == "current") {
					cur = val;
				} else if (sect == "VIDEO") {
					if (key == "layout") {
						Layout lay;
						if (parseLayout(val, lay)) layouts.push_back(lay);
					} else if (key == "bordersize") {
						borderSize = parseNum(val) / 100.0;
					}
				} else if (sect == "ROMSETS") {
					if (key == "name") {
						romsets.push_back(Romset{val, {}});
						rs = &romsets.back();
					} else if (key == "rom" && rs) {
						auto v = split(val, ':');
						RomFile rf;
						rf.name = v.size() > 0 ? v[0] : "";
						rf.foffset = v.size() > 1 ? atoi(v[1].c_str()) : 0;
						rf.fsize = v.size() > 2 ? atoi(v[2].c_str()) : 0;
						rf.roffset = v.size() > 3 ? atoi(v[3].c_str()) : 0;
						if (!rf.name.empty()) rs->roms.push_back(rf);
					}
				}
			});
	}
	if (!cur.empty()) profile = cur;

	// ---- xpeccy-plus machine definitions --------------------------------
	//
	// Plus keeps one file per machine in <conf>/machines/<id>.conf, shipped
	// copies in <src>/res/machines. A user file with the same name is a patch
	// over the shipped one - the GUI writes its per-machine tweaks this way -
	// and `inherit` resolves against the shipped parent, as
	// xcore/machines.cpp:mac_build does. The same file is the only place that
	// says how a machine is really wired (contended memory, snow, floating
	// bus, rom banks), so it is the source a headless server wants as much as
	// the GUI does.

	struct RawMac {
		std::string id;
		std::string parent;
		bool hasInherit = false;	// "inherit =" alone: parent removed, not kept
		// "sect.key" -> value, in the file's own order: upstream applies lines
		// as they come, and keys are not independent - joy.buttons means
		// different things before and after a joy= line, and a sorted map
		// answers a question the file never asked
		std::vector<std::pair<std::string, std::string>> kv;
	};

	std::map<std::string, RawMac> ship, user;
	// one <id>.conf, from a path or from a stream over the compiled-in copy
	auto readMachine = [&](const std::string& f, auto&& src,
			       std::map<std::string, RawMac>& dst) {
		if (f.size() < 6 || f.compare(f.size() - 5, 5, ".conf") != 0) return;
		RawMac raw;
		raw.id = f.substr(0, f.size() - 5);
		parseIniMachine(src,
			[&](const std::string& sect, const std::string& key, const std::string& val) {
				if (sect == "machine" && key == "inherit") {
					raw.parent = val;
					raw.hasInherit = true;
				} else raw.kv.emplace_back(sect + "." + key, val);
			});
		dst[raw.id] = std::move(raw);
	};
	auto scanMachines = [&](const std::string& dir, std::map<std::string, RawMac>& dst) {
		if (!isDir(dir)) return;
		for (const std::string& f : platform::listDir(dir))
			readMachine(f, platform::join(dir, f), dst);
	};

	// interpret one layer of keys into a MachineDef: shipped lines first, then
	// the user's same-named patch on top - mac_apply() in upstream terms
	auto macApply = [](MachineDef& d,
			   const std::vector<std::pair<std::string, std::string>>& kv) {
		// xcore/common.cpp:str2bool - a denylist, not a checklist: anything
		// but an explicit no is yes, so "on" and "banana" are both true
		auto yes = [](const std::string& s) {
			return !(s == "n" || s == "N" || s == "0" || s == "no" || s == "NO" ||
				 s == "false" || s == "FALSE" || s == "off" || s == "OFF");
		};
		// word tables, mirroring xcore/machines.cpp:mac_word() - a value that
		// matches nothing still lands as the table's fallback, not as whatever
		// the inherited half had
		auto word = [](const std::string& s, const char* const* names,
				const int* vals, int count, int fallback, int& out) {
			out = fallback;
			for (int i = 0; i < count; i++)
				if (s == names[i]) { out = vals[i]; return; }
		};
		// upstream parses the arg.i fields with strtol(..., 0): base from
		// the text, so 0x300 is 768K of memory where atoi would say 0.
		// (romN's own offsets are the exception - mac_rom_add uses decimal
		// atoi there, so those stay decimal too)
		auto toInt = [](const std::string& s) {
			return (int)strtol(s.c_str(), nullptr, 0);
		};
		static const char* kResetN[] = {"basic128", "basic48", "dos", "shadow"};
		static const int    kResetV[] = {RES_128, RES_48, RES_DOS, RES_SHADOW};
		static const char* kEarN[]   = {"none", "2", "3"};
		static const int    kEarV[]   = {EAR_NONE, EAR_ISSUE2, EAR_ISSUE3};
		static const char* kFbusN[]  = {"none", "ula", "asic", "attr"};
		static const int    kFbusV[]  = {FBUS_NONE, FBUS_ULA, FBUS_ASIC, FBUS_ATTR};
		// upstream's brdModeTab (xcore/vscalers.cpp) spells the first one "no"
		static const char* kBrdN[]   = {"no", "tiny", "small", "medium", "full", "overscan"};
		static const char* kDiskN[]  = {"none", "trdos", "plus3"};
		static const int    kDiskV[]  = {DIF_NONE, DIF_BDI, DIF_P3DOS};
		static const char* kIdeN[]   = {"none", "nemo", "nemo-a8", "nemo-evo", "smuc", "atm", "profi"};
		static const int    kIdeV[]   = {IDE_NONE, IDE_NEMO, IDE_NEMOA8, IDE_NEMO_EVO, IDE_SMUC, IDE_ATM, IDE_PROFI};
		static const char* kPsgN[]   = {"none", "ay", "ym", "ym2203"};
		static const int    kPsgV[]   = {SND_NONE, SND_AY, SND_YM, SND_YM2203};
		static const char* kStN[]    = {"mono", "abc", "acb", "bac", "bca", "cab", "cba"};
		static const int    kStV[]    = {AY_MONO, AY_ABC, AY_ACB, AY_BAC, AY_BCA, AY_CAB, AY_CBA};
		static const char* kSdrvN[]  = {"none", "covox", "soundrive1", "soundrive2"};
		static const int    kSdrvV[]  = {SDRV_NONE, SDRV_COVOX, SDRV_105_1, SDRV_105_2};
		static const char* kJoyN[]   = {"none", "kempston", "kempston8"};
		static const char* kScanN[]  = {"none", "xt", "at", "ps2"};
		static const int    kScanV[]  = {0, KBD_XT, KBD_AT, KBD_PS2};
		for (const auto& e : kv) {
			const std::string& k = e.first;
			const std::string& v = e.second;
			if (k == "machine.name") d.name = v;
			else if (k == "machine.hw") d.hw = v;
			else if (k == "machine.cpu") d.cpu = v;
			else if (k == "machine.memory") d.memoryKb = toInt(v);
			else if (k == "machine.cpu.frq") d.cpuMhz = toInt(v) / 1e6;
			else if (k == "machine.cpu.turbo") d.turboSteps = v;
			else if (k == "machine.ram.cold") d.ramCold = v;
			else if (k == "machine.ram.noise") {
				int n = toInt(v);
				d.ramNoise = n < 0 ? 0 : (n > 1000 ? 1000 : n);
			}
			else if (k == "machine.reset") word(v, kResetN, kResetV, 4, RES_128, d.resbank);
			else if (k == "machine.issue") word(v, kEarN, kEarV, 3, EAR_ISSUE3, d.earback);
			else if (k == "machine.ear.rc") d.earRC = yes(v);
			else if (k == "machine.contio") d.contio = yes(v);
			else if (k == "machine.contmem") d.contmem = yes(v);
			else if (k == "machine.scrp.wait") d.scrpwait = yes(v);
			else if (k == "video.geometry") d.geometry = v;
			else if (k == "video.contPattern") d.contPattern = toInt(v);
			else if (k == "video.earlyTiming") d.early = yes(v);
			else if (k == "video.4t-border") d.brd4t = yes(v);
			else if (k == "video.snow") d.snow = yes(v);
			else if (k == "video.snow.crash") d.snowx = yes(v);
			else if (k == "video.floatbus") word(v, kFbusN, kFbusV, 4, FBUS_NONE, d.fbus);
			else if (k == "video.border.min") {
				// brd_mode_id(): a name that matches nothing is VID_BRD_FULL
				d.brdmin = VID_BRD_FULL;
				for (int i = 0; i < 6; i++) if (v == kBrdN[i]) d.brdmin = i;
				// what this reader took before it matched upstream, and what
				// every other word table here calls the first entry
				if (v == "none") d.brdmin = VID_BRD_NONE;
			}
			else if (k == "video.ULAplus") d.ulaplus = yes(v);
			else if (k == "video.DDpal") d.ddpal = yes(v);
			else if (k == "storage.disk") word(v, kDiskN, kDiskV, 3, DIF_NONE, d.disk);
			else if (k == "storage.ide") word(v, kIdeN, kIdeV, 7, IDE_NONE, d.ide);
			else if (k == "storage.drives") {
				int n = toInt(v);
				d.drives = n < 1 ? 1 : (n > 4 ? 4 : n);
			}
			else if (k == "sound.psg.count") {
				int n = toInt(v);
				d.psgCount = n < 0 ? 0 : (n > 3 ? 3 : n);
			}
			else if (k == "sound.psg.type") word(v, kPsgN, kPsgV, 4, SND_AY, d.psgType);
			else if (k == "sound.psg.stereo") word(v, kStN, kStV, 7, AY_MONO, d.psgStereo);
			else if (k == "sound.psg.frq") d.psgFrq = parseNum(v);
			else if (k == "sound.soundrive") word(v, kSdrvN, kSdrvV, 4, SDRV_NONE, d.soundrive);
			else if (k == "sound.gs") d.gs = yes(v);
			else if (k == "sound.saa") d.saa = yes(v);
			else if (k == "input.joy") {
				// MAC_JOY_*: the names index straight into the enum
				d.joy = 1;
				for (int i = 0; i < 3; i++) if (v == kJoyN[i]) d.joy = i;
			}
			else if (k == "input.joy.buttons" && d.joy != 0)
				d.joy = yes(v) ? 2 : 1;	// kempston8 / kempston, as upstream
			else if (k == "input.mouse") d.mouse = yes(v);
			else if (k == "input.mouse.wheel") d.mouseWheel = yes(v);
			else if (k == "input.kbd.scantab") word(v, kScanN, kScanV, 4, 0, d.scantab);
			else if (k == "rom.banks") {
				int n = toInt(v);
				d.romBanks = n < 1 ? 1 : (n > 4 ? 4 : n);
			}
			else if (k == "rom.gs") d.gsRom = v;
			else if (k == "rom.font") d.font = v;
			else if (k.compare(0, 7, "rom.rom") == 0 && k.size() > 7 &&
				 isdigit((unsigned char)k[7])) {
				// romN = file[:foffset[:fsize]], offsets in KB; an empty
				// name removes the bank - how a child says "nothing here"
				int roff = atoi(k.c_str() + 7) * 16;
				auto part = split(v, ':');
				auto it = std::find_if(d.roms.begin(), d.roms.end(),
					[roff](const RomFile& r) { return r.roffset == roff; });
				if (part.empty() || part[0].empty()) {
					if (it != d.roms.end()) d.roms.erase(it);
				} else {
					RomFile rf;
					rf.name = part[0];
					if (part.size() > 1) rf.foffset = atoi(part[1].c_str());
					if (part.size() > 2) rf.fsize = atoi(part[2].c_str());
					rf.roffset = roff;
					if (it != d.roms.end()) *it = rf; else d.roms.push_back(rf);
				}
			}
		}
	};

	// The shipped layer is the copy compiled into the binary, never the source
	// tree it was built from: read from there, one executable was a different
	// set of machines depending on whether that tree was still on disk.
	for (const builtin::File& f : builtin::kMachines) {
		std::istringstream in(std::string((const char*)f.data, f.size));
		readMachine(f.name, in, ship);
	}
	scanMachines(platform::join(confDir, "machines"), user);

	// A machine is what it inherits, then what ships under its id, then the
	// user's patch on top - xcore/machines.cpp:mac_build(). What is inherited
	// is always the parent *as it ships*: the user's patch to zx128 must not
	// move zxplus2, which inherits zx128, with it.
	std::map<std::string, MachineDef> stockDone, patchedDone;
	std::function<MachineDef(const std::string&, int, bool)> build =
		[&](const std::string& id, int depth, bool withUser) -> MachineDef {
			std::map<std::string, MachineDef>& done = withUser ? patchedDone : stockDone;
			auto it = done.find(id);
			if (it != done.end()) return it->second;
			const RawMac* s = ship.count(id) ? &ship.at(id) : nullptr;
			const RawMac* u = user.count(id) ? &user.at(id) : nullptr;
			// the user's file has the last word on the parent, like upstream -
			// and "inherit =" with no value removes the shipped one outright,
			// which an absent key and an empty value used to look alike under
			std::string parent = (u && u->hasInherit) ? u->parent
								 : (s ? s->parent : "");
			MachineDef d;
			if (!parent.empty() && depth <= 8 &&
			    (ship.count(parent) || user.count(parent)))
				d = build(parent, depth + 1, false);	// the stock parent
			if (s) macApply(d, s->kv);
			if (withUser && u) macApply(d, u->kv);
			d.id = id;
			if (d.name.empty()) d.name = id;
			done[id] = d;
			return d;
		};
	std::vector<std::string> ids;
	for (const auto& e : ship) ids.push_back(e.first);
	for (const auto& e : user)
		if (!ship.count(e.first)) ids.push_back(e.first);
	for (const auto& id : ids) machines.push_back(build(id, 0, true));
	// the list reads in the order the cores are in (xcore's mac_before()), so
	// a hardware-name lookup lands on the same machine the GUI would pick
	auto coreOrder = [](const std::string& hw) {
		for (int i = 0; tabHwPtr[i].id != HW_NULL; i++)
			if (tabHwPtr[i].core && hw == tabHwPtr[i].core->name) return i;
		return 9999;
	};
	std::sort(machines.begin(), machines.end(),
		[&](const MachineDef& a, const MachineDef& b) {
			int oa = coreOrder(a.hw), ob = coreOrder(b.hw);
			return (oa != ob) ? (oa < ob) : (a.id < b.id);
		});

	// layouts.conf: shipped first, then the user's - findLayout takes the last
	// of a name, which is how the GUI's "a fix in an update reaches a layout
	// nobody has touched" works here too
	auto loadLayouts = [&](auto&& src) {	// a path, or a stream
		parseIni(src, [&](const std::string& sect, const std::string& key, const std::string& val) {
			if (key == "layout") {
				Layout lay;
				if (parseLayout(val, lay)) {
					for (size_t i = 0; i < layouts.size(); i++)
						if (layouts[i].name == lay.name) { layouts[i] = lay; return; }
					layouts.push_back(lay);
				}
			}
		});
	};
	for (const builtin::File& f : builtin::kLayouts) {	// compiled in, as above
		std::istringstream in(std::string((const char*)f.data, f.size));
		loadLayouts(in);
	}
	loadLayouts(platform::join(confDir, "layouts.conf"));

	// the shipped roms are the second place a machine definition looks:
	// upstream copies them into the user's rom directory at first run, which
	// a headless tool must not do - so they are searched, after the user's
	// own directory so a file of the same name there still wins.
	//
	// Compiled in (XSP_BUILTIN_ROM, the default), a name the user's directory
	// does not have is the built-in file of that name. It is settled here,
	// onto the romsets and definitions themselves, because loading reads
	// RomFile::data before it asks any directory. Built without them, the
	// source tree is searched at run time - the only thing that path is still
	// read for, and a copied binary then boots what its own config provides.
#ifdef XSP_BUILTIN_ROMS
	{
		auto attach = [&](std::vector<RomFile>& roms) {
			for (RomFile& rf : roms) {
				if (rf.data || rf.name.empty()) continue;
				if (isFile(platform::join(romDir, rf.name))) continue;
				// built-in names carry no directory, so an absolute or
				// relative path never matches one
				if (const builtin::File* f = builtin::kRoms.find(rf.name)) {
					rf.data = f->data;
					rf.dataSize = (int)f->size;
				}
			}
		};
		for (Romset& rs : romsets) attach(rs.roms);
		for (MachineDef& d : machines) attach(d.roms);
	}
#elif defined(XSP_XPECCY_SRC)
	{
		std::string shipRoms = platform::join(XSP_XPECCY_SRC, "config/roms");
		if (isDir(shipRoms)) {
			romDirShip = shipRoms;
			if (!isDir(romDir)) romDir = shipRoms;
		}
	}
#endif

	// [GENERAL] machine = <machine-id> selects the machine in the plus layout.
	// When it is set it IS the machine - upstream xm_set() puts it straight
	// into conf.machine and the per-profile [MACHINE] current below is the
	// older mechanism, so it must not be allowed to take the name back.
	bool modelLocked = false;
	if (!machines.empty()) {
		std::string genMachine;
		parseIni(platform::join(confDir, "config.conf"),
			[&](const std::string& sect, const std::string& key, const std::string& val) {
				if (sect == "GENERAL" && key == "machine") genMachine = val;
			});
		if (!genMachine.empty()) {
			// the stem goes to setModel as it is: resolving it to a hardware
			// name here and looking the definition up again by that name
			// would land on a different machine when several stems share
			// the hardware ("zxplus2" would come up as "zx128")
			if (const MachineDef* d = findMachine(genMachine)) {
				if (!d->hw.empty()) { model = genMachine; modelLocked = true; }
			} else if (findMachineByHw(genMachine)) {
				model = genMachine;	// a hardware name works too
				modelLocked = true;
			}
		} else if (const MachineDef* d = findMachineByHw(model)) {
			(void)d;	// the configured name already maps to a definition
		}
	}

	// active profile: the machine the GUI would boot - legacy only, so it
	// fills in what [GENERAL] machine did not pin rather than overriding it
	bool profFound = false;
	if (configFound) {
		std::string pf = platform::join(platform::join(prfDir, profile), "xpeccy.conf");
		if (!isFile(pf)) pf = platform::join(confDir, "xpeccy.conf");	// old xpeccy layout
		profFound = parseIni(pf,
			[&](const std::string& sect, const std::string& key, const std::string& val) {
				if (sect == "MACHINE") {
					if (key == "current") {
						if (!modelLocked) model = val;
					}
					else if (key == "memory") memoryKb = atoi(val.c_str());
					else if (key == "cpu.type") cpuType = val;
					else if (key == "cpu.frq") cpuFrq = parseNum(val);
				} else if (sect == "ROMSET") {
					if (key == "current") romsetName = val;
				} else if (sect == "VIDEO") {
					if (key == "geometry" && !val.empty()) layoutName = val;
				}
			});
	}

	// A romset we can actually use as a last resort: the 16K ROM the emulator
	// ships with, from wherever it can still be reached. The two file paths come
	// first, so a host that has the real thing gets the real thing; the copy
	// compiled into this binary is what lets an executable boot after being moved
	// away from the sources it was built against.
	std::string romNote;
	if (!findRomset(romsetName)) {
		Romset rs;
		rs.name = romsetName.empty() ? "ZX48" : romsetName;
		if (isFile(platform::join(romDir, "1982.rom"))) {
			rs.roms.push_back(RomFile{"1982.rom", 0, 16, 0});
			romNote = " [rom: " + romDir + "]";
		}
#if defined(XSP_XPECCY_SRC) && !defined(XSP_BUILTIN_ROMS)
		// xpeccy-plus ships the 48K ROM as config/roms/48.rom, which is the
		// shipped rom directory romPath() already falls back to - so the
		// name alone reaches it, and the user's romDir stays the user's
		else if (!romDirShip.empty() && isFile(platform::join(romDirShip, "48.rom"))) {
			rs.roms.push_back(RomFile{"48.rom", 0, 16, 0});
			romNote = " [rom: Xpeccy sources]";
		}
#endif
#ifdef XSP_BUILTIN_ROM
		else {
			RomFile rf;
			rf.name = "1982.rom (built in)";
			rf.fsize = 16;
			rf.data = kBuiltinRom;
			rf.dataSize = (int)sizeof(kBuiltinRom);
			rs.roms.push_back(rf);
			romNote = " [rom: built in]";
		}
#endif
		if (!rs.roms.empty()) {
			romsets.push_back(rs);
			romsetName = rs.name;
		}
	}

	// The same rescue under the classic name, unconditionally: applyMachineDef
	// falls back to 'ZX48' when the configured romset name exists but every
	// file in it is dead - without this entry that fallback has nowhere to land
	if (!findRomset("ZX48")) {
		Romset rs;
		rs.name = "ZX48";
		if (isFile(platform::join(romDir, "1982.rom"))) {
			rs.roms.push_back(RomFile{"1982.rom", 0, 16, 0});
		}
#if defined(XSP_XPECCY_SRC) && !defined(XSP_BUILTIN_ROMS)
		else if (!romDirShip.empty() && isFile(platform::join(romDirShip, "48.rom"))) {
			rs.roms.push_back(RomFile{"48.rom", 0, 16, 0});
		}
#endif
#ifdef XSP_BUILTIN_ROM
		else {
			RomFile rf;
			rf.name = "1982.rom (built in)";
			rf.fsize = 16;
			rf.data = kBuiltinRom;
			rf.dataSize = (int)sizeof(kBuiltinRom);
			rs.roms.push_back(rf);
		}
#endif
		if (!rs.roms.empty()) romsets.push_back(rs);
	}

	// ...and the same image as a private romset the fallback path applies
	// directly. A user file naming a romset "ZX48" is a legitimate thing to
	// shadow with; it is not a legitimate thing to lose the machine over.
	rescueRom.name = "rescue";
	if (isFile(platform::join(romDir, "1982.rom"))) {
		rescueRom.roms.push_back(RomFile{"1982.rom", 0, 16, 0});
	} else if (!romDirShip.empty() &&
		   isFile(platform::join(romDirShip, "48.rom"))) {
		rescueRom.roms.push_back(RomFile{"48.rom", 0, 16, 0});
	}
#ifdef XSP_BUILTIN_ROM
	// the built-in image stays its own romset rather than joining rescueRom's
	// list: a zero-byte user 1982.rom would shadow it there, and a machine
	// that boots nothing still has this one floor left
	lastRom.name = "builtin";
	{
		RomFile rf;
		rf.name = "1982.rom (built in)";
		rf.fsize = 16;
		rf.data = kBuiltinRom;
		rf.dataSize = (int)sizeof(kBuiltinRom);
		lastRom.roms.push_back(rf);
		if (rescueRom.roms.empty()) rescueRom.roms.push_back(rf);
	}
#endif

	// A model with a machine definition takes its memory, roms and geometry
	// from the definition, and the profile's own values for those are never
	// applied: naming them here made a Pentagon with its own roms read as one
	// running the 48K ROM on the default geometry.
	char buf[512];
	const MachineDef* def = findMachine(model);
	if (!def) def = findMachineByHw(model);
	if (def)
		snprintf(buf, sizeof(buf),
			"config %s (%s), profile '%s'%s: model=%s, machine definition '%s'",
			configFound ? "found" : "missing", confDir.c_str(), profile.c_str(),
			profFound ? "" : " [defaults]", model.c_str(), def->id.c_str());
	else
		snprintf(buf, sizeof(buf),
			"config %s (%s), profile '%s'%s: model=%s memory=%dK romset=%s geometry=%s%s",
			configFound ? "found" : "missing", confDir.c_str(), profile.c_str(),
			profFound ? "" : " [defaults]", model.c_str(), memoryKb,
			romsetName.c_str(), layoutName.c_str(), romNote.c_str());
	note = buf;
}

const Romset* Env::findRomset(const std::string& name) const {
	for (const auto& r : romsets)
		if (r.name == name) return &r;
	return nullptr;
}

const Layout* Env::findLayout(const std::string& name) const {
	const Layout* res = nullptr;
	for (const auto& l : layouts)
		if (l.name == name) res = &l;	// last wins, as in xcore
	return res;
}

const MachineDef* Env::findMachine(const std::string& id) const {
	for (const auto& m : machines)
		if (m.id == id) return &m;
	return nullptr;
}

const MachineDef* Env::findMachineByHw(const std::string& hw) const {
	for (const auto& m : machines)
		if (m.hw == hw) return &m;
	return nullptr;
}

} // namespace xsp
