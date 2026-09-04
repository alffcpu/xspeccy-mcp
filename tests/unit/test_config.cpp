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
}
