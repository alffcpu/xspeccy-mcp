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

// One machine definition from a xpeccy-plus machines/<id>.conf file, with
// `inherit` resolved and the user's same-named patch applied on top. The
// fields hold xcore/machines.cpp:mac_defaults() values unless the definition
// says otherwise - a definition is always complete, so applying one leaves
// nothing of the previous machine standing.
struct MachineDef {
	MachineDef();				// mac_defaults()

	std::string id;			// file stem, e.g. "zx48"
	std::string name;		// [machine] name - display name, defaults to id
	std::string hw;			// libxpeccy hardware name, e.g. "ZX48"
	std::string cpu;		// [machine] cpu - core name
	int memoryKb;			// [machine] memory
	double cpuMhz;			// [machine] cpu.frq, converted Hz -> MHz
	std::string turboSteps;		// [machine] cpu.turbo - board turbo steps, "1,2,4"
	std::string ramCold;		// [machine] ram.cold - cold RAM pattern, hex groups
	int ramNoise;			// [machine] ram.noise - bad cells per 1000 bytes
	int resbank;			// [machine] reset -> RES_*
	int earback;			// [machine] issue -> EAR_*
	int earRC;			// [machine] ear.rc
	int contio;			// [machine] contio
	int contmem;			// [machine] contmem
	int scrpwait;			// [machine] scrp.wait
	int contPattern;		// [video] contPattern
	int early;			// [video] earlyTiming
	int brd4t;			// [video] 4t-border
	int snow;			// [video] snow
	int snowx;			// [video] snow.crash
	int fbus;			// [video] floatbus -> FBUS_*
	int brdmin;			// [video] border.min -> VID_BRD_*
	int ulaplus;			// [video] ULAplus
	int ddpal;			// [video] DDpal
	int disk;			// [storage] disk -> DIF_*
	int ide;			// [storage] ide -> IDE_*
	int drives;			// [storage] drives (1..4)
	std::string geometry;		// [video] geometry -> layout name
	int psgCount;			// [sound] psg.count (0..3)
	int psgType;			// [sound] psg.type -> SND_*
	int psgStereo;			// [sound] psg.stereo -> AY_*
	double psgFrq;			// [sound] psg.frq (MHz)
	int soundrive;			// [sound] soundrive -> SDRV_*
	int gs;				// [sound] gs - General Sound present
	int saa;			// [sound] saa - SAA1099 present
	int joy;			// [input] joy - 0 none, 1 kempston, 2 kempston8
	int mouse;			// [input] mouse
	int mouseWheel;			// [input] mouse.wheel
	int scantab;			// [input] kbd.scantab -> KBD_XT/AT/PS2
	int romBanks;			// [rom] banks - reset-page metadata; the usable
					// size is set by the files that load
	std::vector<RomFile> roms;	// [rom] romN in bank order
	std::string gsRom;		// [rom] gs - GS firmware file name
	std::string font;		// [rom] font - text font file name (tsconf etc.)
};

// Everything we take from the emulator's own configuration.
struct Env {
	std::string confDir;		// ~/.config/xpeccy-plus (or the old samstyle path)
	std::string romDir;		// <confDir>/roms
	std::string romDirShip;		// the roms that ship with the core, searched after romDir
	std::string prfDir;		// <confDir>/profiles

	std::vector<Romset> romsets;
	std::vector<Layout> layouts;
	std::vector<MachineDef> machines;	// plus-format machine definitions
	// the guaranteed-boot rom image (shipped 48.rom, else the compiled-in
	// one). Kept OUT of `romsets` and applied directly, so a user romset
	// that happens to be named "ZX48" cannot shadow the last resort
	Romset rescueRom;
	Romset lastRom;		// the built-in image alone: the floor under rescueRom

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
	// by file stem ("zx48") or by hardware name ("ZX48")
	const MachineDef* findMachine(const std::string& id) const;
	const MachineDef* findMachineByHw(const std::string& hw) const;
};

// helpers shared with other modules
std::string homeDir();
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

} // namespace xsp
