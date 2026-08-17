#include "xsp_config.h"
#include "xsp_platform.h"
#include "xsp_rom_builtin.h"	// generated: defines XSP_BUILTIN_ROM and kBuiltinRom

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace xsp {

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
static bool parseIni(const std::string& path, F&& cb) {
	std::ifstream f(path);
	if (!f.is_open()) return false;
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

	// active profile: the machine the GUI would boot
	bool profFound = false;
	if (configFound) {
		std::string pf = platform::join(platform::join(prfDir, profile), "xpeccy.conf");
		if (!isFile(pf)) pf = platform::join(confDir, "xpeccy.conf");	// old xpeccy layout
		profFound = parseIni(pf,
			[&](const std::string& sect, const std::string& key, const std::string& val) {
				if (sect == "MACHINE") {
					if (key == "current") model = val;
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
#ifdef XSP_XPECCY_SRC
		else if (isFile(platform::join(platform::join(XSP_XPECCY_SRC, "conf"), "1982.rom"))) {
			romDir = platform::join(XSP_XPECCY_SRC, "conf");
			rs.roms.push_back(RomFile{"1982.rom", 0, 16, 0});
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

	char buf[512];
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

} // namespace xsp
