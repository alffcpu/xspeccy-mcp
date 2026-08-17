// Reads Xpeccy's own configuration so that "give me a Pentagon" means exactly
// the machine the user's GUI emulator would give them: same romset, same
// layout, same RAM size. We never write to that config - read only.
#pragma once

#include <string>
#include <vector>

namespace xsp {

// one file inside a romset; offsets/sizes are in KB, as in xpeccy's config.conf
struct RomFile {
	std::string name;
	int foffset = 0;	// offset inside the file
	int fsize = 0;		// bytes to take (0 = whole file)
	int roffset = 0;	// destination offset in ROM space

	// set instead of reading `name` from romDir: the ROM compiled into this
	// binary, which is what makes a copied executable able to boot
	const unsigned char* data = nullptr;
	int dataSize = 0;
};

struct Romset {
	std::string name;
	std::vector<RomFile> roms;
};

// mirrors vLayout (libxpeccy/video/vidcommon.h)
struct Layout {
	std::string name = "default";
	int full_x = 448, full_y = 320;
	int bord_x = 72,  bord_y = 64;
	int blank_x = 64, blank_y = 16;
	int scr_x = 256,  scr_y = 192;
	int int_x = 0,    int_y = 0;
	int int_size = 64;
};

// Everything we take from the emulator's own configuration.
struct Env {
	std::string confDir;		// ~/.config/samstyle/xpeccy
	std::string romDir;		// <confDir>/roms
	std::string prfDir;		// <confDir>/profiles

	std::vector<Romset> romsets;
	std::vector<Layout> layouts;

	// defaults taken from the active profile (profiles/<p>/xpeccy.conf)
	std::string profile = "default";
	std::string model = "Pentagon";		// [MACHINE] current
	int memoryKb = 128;			// [MACHINE] memory
	std::string cpuType = "Z80";		// [MACHINE] cpu.type
	double cpuFrq = 3500000.0;		// [MACHINE] cpu.frq
	std::string romsetName = "ZX48";	// [ROMSET] current
	std::string layoutName = "default";	// [VIDEO] geometry
	double borderSize = 0.5;		// [VIDEO] bordersize / 100

	bool configFound = false;		// false => built-in fallbacks are in use
	std::string note;			// human-readable summary of what was loaded

	// Loads config.conf + the active profile. Never fails hard: when nothing is
	// found, built-in defaults are used and `note` explains it.
	void load(const std::string& forcedDir = "");

	const Romset* findRomset(const std::string& name) const;
	const Layout* findLayout(const std::string& name) const;
};

// helpers shared with other modules
std::string homeDir();
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

} // namespace xsp
