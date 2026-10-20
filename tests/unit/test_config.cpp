// Reading the emulator's own configuration, which is what makes "give me a
// Pentagon" mean the same machine here as in the user's GUI. Everything in this
// file is a parser for a format we do not control, so the tests are written
// against fixtures that look exactly like the files xpeccy writes.
#include "unit.h"
#include "xsp_config.h"
#include "xsp_platform.h"

#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "video/video.h"	// VID_BRD_* for border.min
}

using namespace xsp;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
	_putenv_s(name, value ? value : "");
#else
	if (value && *value) setenv(name, value, 1);
	else unsetenv(name);
#endif
}

std::string getEnv(const char* name) {
	const char* v = getenv(name);
	return v ? v : "";
}

// Points every place the loader looks at an empty directory, so that "no
// configuration" means no configuration and not "whatever this developer's
// machine happens to have in it". A test that reads the host passes here and
// fails on the build machine, which is the worst of both.
class NoHostConfig {
public:
	explicit NoHostConfig(const std::string& emptyHome)
		: m_home(getEnv("HOME")), m_xdg(getEnv("XDG_CONFIG_HOME")),
		  m_forced(getEnv("XSP_XPECCY_CONF")) {
		setEnv("HOME", emptyHome.c_str());
		setEnv("XDG_CONFIG_HOME", nullptr);
		setEnv("XSP_XPECCY_CONF", nullptr);
	}
	~NoHostConfig() {
		setEnv("HOME", m_home.c_str());
		setEnv("XDG_CONFIG_HOME", m_xdg.c_str());
		setEnv("XSP_XPECCY_CONF", m_forced.c_str());
	}
private:
	std::string m_home, m_xdg, m_forced;
};

} // namespace

void test_config() {
	unit::begin("config");

	// ---- trim -------------------------------------------------------------
	//
	// Config lines arrive with trailing \r on a file written under Windows, and
	// a key that keeps it matches nothing.
	{
		CHECK_EQ(trim("  hello  "), std::string("hello"));
		CHECK_EQ(trim("\t hello \r\n"), std::string("hello"));
		CHECK_EQ(trim("hello"), std::string("hello"));
		CHECK_EQ(trim(""), std::string(""));
		CHECK_EQ(trim("   "), std::string(""));
		CHECK_EQ(trim("\r\n\t "), std::string(""));
		// Inner spacing is not whitespace to be tidied: a romset can be called
		// "ZX 48".
		CHECK_EQ(trim("  a b  "), std::string("a b"));
	}

	// ---- split ------------------------------------------------------------
	//
	// Colon-separated, and the empty fields are meaningful: a layout line with a
	// blank in the middle must not shift every later field one place left.
	{
		std::vector<std::string> three = split("a:b:c", ':');
		CHECK_EQ((int)three.size(), 3);
		CHECK_EQ(three[0], std::string("a"));
		CHECK_EQ(three[2], std::string("c"));

		std::vector<std::string> gap = split("a::c", ':');
		CHECK_EQ((int)gap.size(), 3);
		CHECK_EQ(gap[1], std::string(""));

		// A string with no separator is one field, not none.
		std::vector<std::string> one = split("solo", ':');
		CHECK_EQ((int)one.size(), 1);
		CHECK_EQ(one[0], std::string("solo"));

		// A trailing separator makes a trailing empty field, which is how
		// "1982.rom:0:16:" is meant to read.
		std::vector<std::string> trailing = split("a:", ':');
		CHECK_EQ((int)trailing.size(), 2);
		CHECK_EQ(trailing[1], std::string(""));

		std::vector<std::string> empty = split("", ':');
		CHECK_EQ((int)empty.size(), 1);
		CHECK_EQ(empty[0], std::string(""));
	}

	// ---- a whole configuration tree ---------------------------------------
	//
	// The shape xpeccy actually writes: config.conf beside a profiles/
	// directory, and the profile named by [PROFILES] current is the one that
	// decides which machine boots.
	{
		unit::TempDir tmp("config");
		tmp.file("config.conf",
			 "# a comment, and a blank line follow\n"
			 "\n"
			 "[PROFILES]\n"
			 "current = work\n"
			 "[VIDEO]\n"
			 "bordersize = 62\n"
			 "layout = Pentagon:448:320:72:48:64:32:64:0:0:256:192\n"
			 "layout = Narrow:320:240:32:24:32:16:32:0:0:256:192\n"
			 "[ROMSETS]\n"
			 "name = ZX48\n"
			 "rom = 1982.rom:0:16:0\n"
			 "name = Two-part\n"
			 "rom = first.rom:0:8:0\n"
			 "rom = second.rom:8:8:16\n");
		tmp.file("profiles/work/xpeccy.conf",
			 "[MACHINE]\n"
			 "current = ZX Scorpion\n"
			 "memory = 256\n"
			 "cpu.type = Z80\n"
			 "cpu.frq = 7,000000\n"
			 "[ROMSET]\n"
			 "current = Two-part\n"
			 "[VIDEO]\n"
			 "geometry = Narrow\n");
		// an empty roms directory: enough to keep the shipped-ROMs fallback
		// from redirecting romDir into the emulator sources
		tmp.file("roms/.keep", "");

		Env env;
		env.load(tmp.path());

		CHECK(env.configFound);
		CHECK_EQ(env.confDir, tmp.path());
		CHECK_EQ(env.romDir, platform::join(tmp.path(), "roms"));
		CHECK_EQ(env.prfDir, platform::join(tmp.path(), "profiles"));

		// The profile named in config.conf is the one that was read, and its
		// values are the ones in force.
		CHECK_EQ(env.profile, std::string("work"));
		CHECK_EQ(env.model, std::string("ZX Scorpion"));
		CHECK_EQ(env.memoryKb, 256);
		CHECK_EQ(env.romsetName, std::string("Two-part"));
		CHECK_EQ(env.layoutName, std::string("Narrow"));

		// xpeccy writes doubles through the C locale of whoever ran it, so a
		// comma is a decimal point. Reading 7,000000 as 7 rather than as 7000000
		// or as 0 is the difference between a working clock and nonsense.
		CHECK(env.cpuFrq > 6.99 && env.cpuFrq < 7.01);
		CHECK(env.borderSize > 0.61 && env.borderSize < 0.63);

		// ---- romsets ---------------------------------------------------
		//
		// Each `name` starts a set and the `rom` lines after it belong to it.
		// Getting this wrong puts every ROM into the first set, which then
		// boots and looks almost right.
		const Romset* zx48 = env.findRomset("ZX48");
		const Romset* two = env.findRomset("Two-part");
		CHECK(zx48 != nullptr);
		CHECK(two != nullptr);
		CHECK(env.findRomset("nothing") == nullptr);
		if (zx48) CHECK_EQ((int)zx48->roms.size(), 1);
		if (two) {
			CHECK_EQ((int)two->roms.size(), 2);
			CHECK_EQ(two->roms[0].name, std::string("first.rom"));
			CHECK_EQ(two->roms[1].name, std::string("second.rom"));
			CHECK_EQ(two->roms[1].foffset, 8);
			CHECK_EQ(two->roms[1].fsize, 8);
			CHECK_EQ(two->roms[1].roffset, 16);
		}

		// ---- layouts ---------------------------------------------------
		//
		// The field order in a layout line is not the order of the struct, and
		// nothing about the line says so: int_size comes before int_y, and
		// int_x is after both. Anything that reads them in declaration order
		// produces a picture with the interrupt in the wrong place, which looks
		// like a timing bug somewhere else entirely.
		const Layout* pent = env.findLayout("Pentagon");
		CHECK(pent != nullptr);
		if (pent) {
			CHECK_EQ(pent->full_x, 448);
			CHECK_EQ(pent->full_y, 320);
			CHECK_EQ(pent->bord_x, 72);
			CHECK_EQ(pent->bord_y, 48);
			CHECK_EQ(pent->blank_x, 64);
			CHECK_EQ(pent->blank_y, 32);
			CHECK_EQ(pent->int_size, 64);
			CHECK_EQ(pent->int_y, 0);
			CHECK_EQ(pent->int_x, 0);
			CHECK_EQ(pent->scr_x, 256);
			CHECK_EQ(pent->scr_y, 192);
		}

		// The built-in "default" is always there, so a config with no layouts
		// at all still has one to name.
		CHECK(env.findLayout("default") != nullptr);
		CHECK(env.findLayout("Narrow") != nullptr);
		CHECK(env.findLayout("no such layout") == nullptr);

		CHECK(env.note.find("found") != std::string::npos);
		CHECK(env.note.find("ZX Scorpion") != std::string::npos);
	}

	// ---- a layout line that is too short ----------------------------------
	//
	// Nine fields is the minimum; the last three have defaults. A shorter line
	// is dropped rather than half-read, because a layout with a zero width
	// crashes the video code rather than looking wrong.
	{
		unit::TempDir tmp("config-short");
		tmp.file("config.conf",
			 "[VIDEO]\n"
			 "layout = Broken:448:320\n"
			 "layout = Minimal:448:320:72:48:64:32:64:0\n");
		Env env;
		env.load(tmp.path());
		CHECK(env.findLayout("Broken") == nullptr);
		const Layout* minimal = env.findLayout("Minimal");
		CHECK(minimal != nullptr);
		if (minimal) {
			CHECK_EQ(minimal->int_x, 0);
			CHECK_EQ(minimal->scr_x, 256);	// defaulted, not left at zero
			CHECK_EQ(minimal->scr_y, 192);
		}
	}

	// A layout wider than the core's own buffer is clamped rather than trusted:
	// the frame buffer is a fixed 512 across and a larger number writes past it.
	{
		unit::TempDir tmp("config-huge");
		tmp.file("config.conf",
			 "[VIDEO]\n"
			 "layout = Huge:9999:8888:72:48:64:32:64:0:0:256:192\n");
		Env env;
		env.load(tmp.path());
		const Layout* huge = env.findLayout("Huge");
		CHECK(huge != nullptr);
		if (huge) {
			CHECK_EQ(huge->full_x, 512);
			CHECK_EQ(huge->full_y, 512);
		}
	}

	// Two layouts of the same name: the last one wins, as it does in xcore, so
	// that a user's own line added at the end of the file takes effect.
	{
		unit::TempDir tmp("config-dup");
		tmp.file("config.conf",
			 "[VIDEO]\n"
			 "layout = Same:448:320:72:48:64:32:64:0:0:256:192\n"
			 "layout = Same:320:240:32:24:32:16:32:0:0:256:192\n");
		Env env;
		env.load(tmp.path());
		const Layout* same = env.findLayout("Same");
		CHECK(same != nullptr);
		if (same) CHECK_EQ(same->full_x, 320);
	}

	// ---- the old single-file layout ---------------------------------------
	//
	// Before profiles existed the machine lived in confDir/xpeccy.conf. A user
	// upgrading from that version still has one, and it is still readable.
	{
		unit::TempDir tmp("config-old");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("xpeccy.conf",
			 "[MACHINE]\n"
			 "current = ZX48K\n"
			 "memory = 48\n");
		Env env;
		env.load(tmp.path());
		CHECK_EQ(env.model, std::string("ZX48K"));
		CHECK_EQ(env.memoryKb, 48);
	}

	// ---- no configuration at all ------------------------------------------
	//
	// The server has to start on a machine that has never run the emulator, and
	// say so rather than pretending. It still needs a bootable romset, which is
	// what the ROM compiled into the binary is for.
	{
		unit::TempDir tmp("config-none");
		NoHostConfig isolated(tmp.path());
		Env env;
		env.load(platform::join(tmp.path(), "does-not-exist"));
		CHECK(!env.configFound);
		CHECK_EQ(env.model, std::string("Pentagon"));	// the built-in defaults
		CHECK_EQ(env.memoryKb, 128);
		CHECK_EQ(env.layoutName, std::string("default"));
		CHECK(env.findLayout("default") != nullptr);
		CHECK(env.note.find("missing") != std::string::npos);

		// Whatever the fallback found - a file beside the binary, the Xpeccy
		// sources or the built-in copy - the named romset has to exist and
		// contain something, or the machine cannot boot at all.
		const Romset* fallback = env.findRomset(env.romsetName);
		CHECK(fallback != nullptr);
		if (fallback) {
			CHECK(!fallback->roms.empty());
			const RomFile& rom = fallback->roms.front();
			CHECK(rom.data != nullptr || !rom.name.empty());
			CHECK_EQ(rom.fsize, 16);		// a 16K ROM, however it was found
		}
	}

	// A forced directory that does not exist falls back to wherever the host
	// keeps its own configuration. That is what Env::load does today and the
	// test says so rather than wishing otherwise: refusing it is main()'s job,
	// because only main() knows the directory came from --config rather than
	// from the search list.
	{
		unit::TempDir host("config-host");
		host.file("config.conf", "[PROFILES]\ncurrent = hostprofile\n");
		host.file("profiles/hostprofile/xpeccy.conf", "[MACHINE]\ncurrent = ZX48K\n");

		const std::string saved = getEnv("XSP_XPECCY_CONF");
		setEnv("XSP_XPECCY_CONF", host.path().c_str());
		Env env;
		env.load(platform::join(host.path(), "not-a-directory"));
		CHECK(env.configFound);
		CHECK_EQ(env.confDir, host.path());
		CHECK_EQ(env.model, std::string("ZX48K"));
		setEnv("XSP_XPECCY_CONF", saved.c_str());
	}

	// ---- machine definitions (machines/<id>.conf) ---------------------------
	//
	// The dispatch over "sect.key" is one long else-if chain, and the parse of
	// every section is checked, not just the first ones: a branch written
	// without braces once swallowed the rest of the chain wholesale, which left
	// [storage], [sound], [input] and all of [rom] silently unapplied while
	// every machine still "worked" on its hardware defaults.
	{
		unit::TempDir tmp("config-machines");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("machines/base.conf",
			 "[machine]\n"
			 "name = Base Machine\n"
			 "hw = ZX48\n"
			 "memory = 128\n"
			 "cpu.frq = 3500000\n"
			 "contmem = yes\n"
			 "scrp.wait = no\n"
			 "[video]\n"
			 "geometry = ULA.48\n"
			 "border.min = small\n"
			 "floatbus = ula\n"
			 "[storage]\n"
			 "disk = trdos\n"
			 "ide = nemo\n"
			 "drives = 2\n"
			 "[sound]\n"
			 "psg.count = 2\n"
			 "psg.type = ym\n"
			 "psg.stereo = abc\n"
			 "soundrive = covox\n"
			 "[input]\n"
			 "mouse = yes\n"
			 "mouse.wheel = yes\n"
			 "joy = kempston\n"
			 "kbd.scantab = at\n"
			 "[rom]\n"
			 "rom0 = base0.rom\n"
			 "rom1 = base1.rom:16:16\n");
		// a child: own keys win, the parent's survive, a rom line with an
		// empty name deletes the parent's bank
		tmp.file("machines/child.conf",
			 "[machine]\n"
			 "inherit = base\n"
			 "name = Child Machine\n"
			 "memory = 512\n"
			 "[rom]\n"
			 "rom1 = \n"
			 "rom2 = child2.rom:8:32\n");

		Env env;
		env.load(tmp.path());
		const MachineDef* base = env.findMachine("base");
		const MachineDef* child = env.findMachine("child");
		CHECK(base != nullptr);
		CHECK(child != nullptr);
		if (base) {
			CHECK_EQ(base->hw, std::string("ZX48"));
			CHECK_EQ(base->memoryKb, 128);
			CHECK_EQ(base->contmem, 1);
			CHECK_EQ(base->scrpwait, 0);	// explicit no, not unset
			CHECK_EQ(base->geometry, std::string("ULA.48"));
			CHECK(base->fbus != -1);	// FBUS_ULA
			CHECK(base->brdmin != -1);
			CHECK(base->disk != -1);	// DIF_BDI
			CHECK(base->ide != -1);		// IDE_NEMO
			CHECK_EQ(base->drives, 2);
			CHECK_EQ(base->psgCount, 2);
			CHECK(base->psgType != -1);	// SND_YM
			CHECK(base->psgStereo != -1);	// AY_ABC
			CHECK(base->soundrive != -1);	// SDRV_COVOX
			CHECK_EQ(base->mouse, 1);
			CHECK_EQ(base->mouseWheel, 1);
			CHECK_EQ(base->joy, 1);		// kempston
			CHECK(base->scantab != -1);	// KBD_AT
			CHECK_EQ((int)base->roms.size(), 2);
			if (base->roms.size() == 2) {
				CHECK_EQ(base->roms[0].name, std::string("base0.rom"));
				CHECK_EQ(base->roms[0].roffset, 0);
				CHECK_EQ(base->roms[1].name, std::string("base1.rom"));
				CHECK_EQ(base->roms[1].roffset, 16);	// bank 1 -> 16K
				CHECK_EQ(base->roms[1].foffset, 16);	// KB into the file
				CHECK_EQ(base->roms[1].fsize, 16);
			}
		}
		if (child) {
			// What is inherited is the parent *as it ships* - and a user-only
			// parent ships nothing, so its own keys never cross. In upstream
			// this is fine because xm_save_as writes a child file carrying
			// every key that differs from the stock grandparent; a hand-
			// written chain gets the same rule: its own keys over the
			// defaults, and nothing of base's.
			CHECK_EQ(child->memoryKb, 512);		// its own
			CHECK_EQ(child->name, std::string("Child Machine"));
			CHECK(child->hw.empty());		// the defaults carry no hardware
			CHECK_EQ(child->contmem, 0);		// defaults, not base's yes
			CHECK_EQ(child->drives, 4);		// defaults, not base's 2
			// only rom2: base's banks never arrived, so rom1's empty name
			// has nothing to delete
			CHECK_EQ((int)child->roms.size(), 1);
			if (child->roms.size() == 1) {
				CHECK_EQ(child->roms[0].name, std::string("child2.rom"));
				CHECK_EQ(child->roms[0].roffset, 32);
				CHECK_EQ(child->roms[0].foffset, 8);
				CHECK_EQ(child->roms[0].fsize, 32);
			}
		}
		CHECK(env.findMachine("no-such-machine") == nullptr);
	}

	// ---- ints take strtol-0, rom offsets stay decimal ----------------------
	//
	// machine.memory, cpu.frq, drives and friends parse like upstream's arg.i
	// fields: strtol with base 0, so 0x is hex and a leading 0 is octal. The
	// romN fields are the exception - upstream mac_rom_add runs decimal atoi
	// on them, so 016 in a rom line is sixteen, not fourteen.
	{
		unit::TempDir tmp("config-strtol0");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("machines/hexed.conf",
			 "[machine]\n"
			 "name = Hexed\n"
			 "hw = ZX48\n"
			 "memory = 0x100\n"
			 "cpu.frq = 0x3567E0\n"
			 "[storage]\n"
			 "drives = 0x2\n"
			 "[rom]\n"
			 "rom0 = r0.rom:016:8\n");

		Env env;
		env.load(tmp.path());
		const MachineDef* hexed = env.findMachine("hexed");
		CHECK(hexed != nullptr);
		if (hexed) {
			CHECK_EQ(hexed->memoryKb, 256);
			CHECK_EQ(hexed->drives, 2);
			CHECK(hexed->cpuMhz > 3.4 && hexed->cpuMhz < 3.6);
			CHECK_EQ((int)hexed->roms.size(), 1);
			if (hexed->roms.size() == 1)
				CHECK_EQ(hexed->roms[0].foffset, 16);
		}
	}

	// ---- a user file is a patch, not a replacement -------------------------
	//
	// The GUI writes its per-machine tweaks into confDir/machines/<id>.conf
	// holding only the keys that changed. Layered over the shipped definition
	// of the same stem, or every machine the user ever touched would lose its
	// roms, its hardware name and its geometry - xcore/machines.cpp:mac_build.
	// And `inherit` resolves against the parent *as it ships*: a patch to the
	// parent must not move a machine that inherits it.
#ifdef XSP_XPECCY_SRC
	{
		unit::TempDir tmp("config-patch");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("machines/zx48.conf",
			 "[machine]\n"
			 "memory = 96\n"
			 "contmem = no\n");
		tmp.file("machines/mine.conf",
			 "[machine]\n"
			 "inherit = zx48\n");

		Env env;
		env.load(tmp.path());
		const MachineDef* z = env.findMachine("zx48");
		CHECK(z != nullptr);
		if (z) {
			CHECK_EQ(z->memoryKb, 96);		// the patch's own key
			CHECK_EQ(z->contmem, 0);		// the patch's own key
			CHECK_EQ(z->hw, std::string("ZX48"));	// shipped, untouched
			CHECK_EQ(z->name, std::string("ZX Spectrum 48K"));
			CHECK(z->roms.size() >= 1);		// shipped rom0 still there
		}
		const MachineDef* mine = env.findMachine("mine");
		CHECK(mine != nullptr);
		if (mine) {
			// stock zx48 has memory=64 - the user's 96 must not leak in
			CHECK_EQ(mine->memoryKb, 64);
			CHECK_EQ(mine->contmem, 1);		// stock zx48's own yes
			// and the inherited name stays - the id is only a fallback
			// when nothing in the whole chain named the machine
			CHECK_EQ(mine->name, std::string("ZX Spectrum 48K"));
		}
	}
#endif

	// ---- [GENERAL] machine names the boot machine --------------------------
	//
	// The key takes a definition stem or a hardware name - the GUI writes
	// whichever its machine list holds. Either has to resolve, and a stem
	// must stay a stem or "zxplus2" would quietly come up as zx128.
#ifdef XSP_XPECCY_SRC
	{
		unit::TempDir tmp("config-general-hw");
		tmp.file("config.conf",
			 "[GENERAL]\n"
			 "machine = Scorpion\n");
		Env env;
		env.load(tmp.path());
		CHECK_EQ(env.model, std::string("Scorpion"));	// the hw name, resolved
	}
	{
		unit::TempDir tmp("config-general-stem");
		tmp.file("config.conf",
			 "[GENERAL]\n"
			 "machine = zxplus2\n");
		Env env;
		env.load(tmp.path());
		CHECK_EQ(env.model, std::string("zxplus2"));	// the stem, kept
	}
#endif

	// ---- machine files speak the upstream dialect ----------------------------
	//
	// xcore's mac_read strips an inline comment wherever a '#' or ';' stands,
	// reads section names case-blind and answers "is this bool" with a
	// denylist: only n/N/0/no/false/off is false, everything else is yes.
	// A parser that treats "yes # enabled" as false makes the shipped files
	// lie about the machines they describe.
	{
		unit::TempDir tmp("config-maccfg");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("machines/dialect.conf",
			 "[MACHINE]\n"					// uppercase section, like upstream writes
			 "hw = ZX48\n"
			 "contmem = yes # enabled on this board\n"
			 "scrp.wait = banana ; anything but a clear no is yes\n"
			 "contio = off\n"
			 "memory = 64 # kilobytes\n"
			 "[storage]\n"
			 "disk = trdos ; the beta interface\n");
		Env env;
		env.load(tmp.path());
		const MachineDef* d = env.findMachine("dialect");
		CHECK(d != nullptr);
		if (d) {
			CHECK_EQ(d->contmem, 1);		// "yes # comment" is yes
			CHECK_EQ(d->scrpwait, 1);		// denylist: "banana" is not a no
			CHECK_EQ(d->contio, 0);			// "off" is the no it means
			CHECK_EQ(d->memoryKb, 64);		// the comment is gone before atoi
			CHECK_EQ(d->disk, 1);			// DIF_BDI
		}
	}

	// An empty `inherit =` is still the inherit key: the GUI clears a shipped
	// parent that way, and an absent value must not read as "key not there".
#ifdef XSP_XPECCY_SRC
	{
		unit::TempDir tmp("config-noinherit");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		// profi ships inheriting profi3 - its Beta Disk comes from the
		// parent, while hw and memory are its own keys. Clearing the link
		// drops the parent only: the file's own lines still apply.
		tmp.file("machines/profi.conf",
			 "[machine]\n"
			 "inherit =\n");
		Env env;
		env.load(tmp.path());
		const MachineDef* d = env.findMachine("profi");
		CHECK(d != nullptr);
		if (d) {
			CHECK_EQ(d->hw, std::string("Profi"));	// its own key survives
			CHECK_EQ(d->memoryKb, 1024);		// its own key survives
			CHECK_EQ(d->disk, 0);			// DIF_NONE: profi3's, gone
		}
	}
#endif

	// ---- [GENERAL] machine outranks the legacy profile ----------------------
	//
	// config.conf's [GENERAL] machine is the schema-2 way of saying which
	// machine runs; the profile's [MACHINE] current is the older schema. When
	// both say something, the newer key wins or a config written by the new
	// GUI silently boots the old profile's machine instead.
	{
		unit::TempDir tmp("config-priority");
		tmp.file("config.conf",
			 "[PROFILES]\n"
			 "current = work\n"
			 "[GENERAL]\n"
			 "machine = Scorpion\n");
		// a definition for the name to resolve against - [GENERAL] machine
		// acts only when some machine list exists to name one
		tmp.file("machines/zs.conf",
			 "[machine]\n"
			 "hw = Scorpion\n");
		tmp.file("profiles/work/xpeccy.conf",
			 "[MACHINE]\n"
			 "current = ZX48K\n");
		Env env;
		env.load(tmp.path());
		CHECK_EQ(env.model, std::string("Scorpion"));	// GENERAL, not the profile's
	}

	// Lines that are not settings are skipped rather than half-parsed: comments
	// with either marker, blanks, and a line with no '=' at all.
	{
		unit::TempDir tmp("config-junk");
		tmp.file("config.conf",
			 "# hash comment\n"
			 "; semicolon comment\n"
			 "\n"
			 "   \n"
			 "this line has no equals sign\n"
			 "[PROFILES]\n"
			 "current = ok\n");
		Env env;
		env.load(tmp.path());
		CHECK_EQ(env.profile, std::string("ok"));
	}

	// ---- a machine file saved with a byte order mark -----------------------
	//
	// Upstream reads through QTextStream, which drops a leading UTF-8 BOM. Left
	// in, it sits in front of "[machine]", the line stops being a section header,
	// and every key under it lands in no section - the machine comes up as the
	// defaults, with no hardware at all.
	{
		unit::TempDir tmp("config-bom");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("machines/bom.conf",
			 "\xEF\xBB\xBF[machine]\n"
			 "hw = ZX48\n"
			 "memory = 256\n");
		Env env;
		env.load(tmp.path());
		const MachineDef* bom = env.findMachine("bom");
		CHECK(bom != nullptr);
		if (bom) {
			CHECK_EQ(bom->hw, std::string("ZX48"));
			CHECK_EQ(bom->memoryKb, 256);
		}
	}

	// ---- border.min spells "no" the way upstream does ----------------------
	//
	// brdModeTab (xcore/vscalers.cpp) names VID_BRD_NONE "no", and a name that
	// matches nothing is VID_BRD_FULL - so a file the GUI wrote with "no" used
	// to come up with the biggest border instead of none. "none" stays a
	// synonym, since it is what this reader used to want.
	{
		unit::TempDir tmp("config-brdmin");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("machines/brdno.conf", "[machine]\nhw = ZX48\n[video]\nborder.min = no\n");
		tmp.file("machines/brdnone.conf", "[machine]\nhw = ZX48\n[video]\nborder.min = none\n");
		tmp.file("machines/brdjunk.conf", "[machine]\nhw = ZX48\n[video]\nborder.min = huge\n");
		Env env;
		env.load(tmp.path());
		const MachineDef* no = env.findMachine("brdno");
		const MachineDef* none = env.findMachine("brdnone");
		const MachineDef* junk = env.findMachine("brdjunk");
		CHECK(no && none && junk);
		if (no) CHECK_EQ(no->brdmin, (int)VID_BRD_NONE);
		if (none) CHECK_EQ(none->brdmin, (int)VID_BRD_NONE);
		if (junk) CHECK_EQ(junk->brdmin, (int)VID_BRD_FULL);
	}
}
