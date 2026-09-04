// The layer that hides Linux, macOS and Windows from the rest of the server.
// It is small, which is the reason it is easy to get wrong: every function here
// is a one-liner whose failure mode is a path that looks right and points
// nowhere, and the server then reports "config missing" instead of a mistake.
#include "unit.h"
#include "xsp_platform.h"

#include <cstdlib>
#include <string>

using namespace xsp;

namespace {

// setenv/unsetenv are POSIX; Windows spells the same thing _putenv_s.
void setEnv(const char* name, const char* value) {
#ifdef _WIN32
	_putenv_s(name, value ? value : "");
#else
	if (value) setenv(name, value, 1);
	else unsetenv(name);
#endif
}

std::string getEnv(const char* name) {
	const char* v = getenv(name);
	return v ? v : "";
}

const char kSep =
#ifdef _WIN32
	'\\';
#else
	'/';
#endif

} // namespace

void test_platform() {
	unit::begin("platform");

	// ---- join ------------------------------------------------------------
	//
	// One separator, never two, and never none. Doubling it is harmless on
	// Unix and not on every Windows API, and leaving it out silently
	// concatenates two names into a third that does not exist.
	{
		CHECK_EQ(platform::join("a", "b"), std::string("a") + kSep + "b");
		CHECK_EQ(platform::join("a/", "b"), std::string("a/b"));
		CHECK_EQ(platform::join("a\\", "b"), std::string("a\\b"));

		// An empty side means there is nothing to join, so the other side
		// comes back untouched rather than gaining a leading separator.
		CHECK_EQ(platform::join("", "b"), std::string("b"));
		CHECK_EQ(platform::join("a", ""), std::string("a"));
		CHECK_EQ(platform::join("", ""), std::string(""));

		// Nesting is how every config path in the server is built.
		CHECK_EQ(platform::join(platform::join("a", "b"), "c"),
			 std::string("a") + kSep + "b" + kSep + "c");
	}

	// ---- isDir / isFile / makeDir ----------------------------------------
	//
	// These are three different questions and the server asks them of the same
	// path in different places: a file where a directory is expected has to
	// answer no, not "it exists".
	{
		unit::TempDir tmp("platform");
		CHECK(platform::isDir(tmp.path()));
		CHECK(!platform::isFile(tmp.path()));

		const std::string file = tmp.file("a-file.txt", "hello");
		CHECK(platform::isFile(file));
		CHECK(!platform::isDir(file));

		const std::string missing = platform::join(tmp.path(), "not-here");
		CHECK(!platform::isFile(missing));
		CHECK(!platform::isDir(missing));

		// makeDir is expected to be idempotent: the server calls it on a path
		// that usually already exists.
		const std::string made = platform::join(tmp.path(), "made");
		CHECK(platform::makeDir(made));
		CHECK(platform::isDir(made));
		CHECK(platform::makeDir(made));
		CHECK(platform::isDir(made));
		CHECK_EQ(tmp.dir("made"), made);	// hands it to the cleanup list

		// One level only: makeDir is not mkdir -p, and a caller that assumes
		// otherwise would get a silent false rather than a tree.
		CHECK(!platform::makeDir(platform::join(made, "x/y/z")));
	}

	// ---- tempDir ----------------------------------------------------------
	//
	// TMPDIR first, then TEMP, then TMP, and a real directory when none is set.
	// The order matters because macOS sets only TMPDIR and Windows only TEMP.
	{
		const std::string savedTmpdir = getEnv("TMPDIR");
		const std::string savedTemp = getEnv("TEMP");
		const std::string savedTmp = getEnv("TMP");

		setEnv("TMPDIR", "/first");
		setEnv("TEMP", "/second");
		setEnv("TMP", "/third");
		CHECK_EQ(platform::tempDir(), std::string("/first"));

		setEnv("TMPDIR", "");
		CHECK_EQ(platform::tempDir(), std::string("/second"));

		setEnv("TEMP", "");
		CHECK_EQ(platform::tempDir(), std::string("/third"));

		// Nothing set at all still has to name a directory that exists, or
		// every screenshot the server writes lands nowhere.
		setEnv("TMP", "");
		CHECK(platform::isDir(platform::tempDir()));

		setEnv("TMPDIR", savedTmpdir.empty() ? nullptr : savedTmpdir.c_str());
		setEnv("TEMP", savedTemp.empty() ? nullptr : savedTemp.c_str());
		setEnv("TMP", savedTmp.empty() ? nullptr : savedTmp.c_str());
	}

	// ---- homeDir ----------------------------------------------------------
	{
		const std::string saved = getEnv("HOME");
		setEnv("HOME", "/somewhere/else");
		CHECK_EQ(platform::homeDir(), std::string("/somewhere/else"));
		setEnv("HOME", saved.empty() ? nullptr : saved.c_str());
		CHECK(!platform::homeDir().empty());
	}

	// ---- where Xpeccy's configuration is looked for -----------------------
	//
	// XSP_XPECCY_CONF is an override, so it has to come first: a caller who
	// sets it is telling the server which machine to be, and a candidate ahead
	// of it would silently win.
	{
		const std::string savedForce = getEnv("XSP_XPECCY_CONF");
		const std::string savedHome = getEnv("HOME");
		const std::string savedXdg = getEnv("XDG_CONFIG_HOME");

		setEnv("XSP_XPECCY_CONF", "");
		// Not shaped like a real home directory on purpose: the publisher
		// scans the public tree for absolute /home and /Users paths, and a
		// fixture that trips that guard teaches everyone to ignore it.
		setEnv("HOME", "/test-home");
		setEnv("XDG_CONFIG_HOME", "");

		std::vector<std::string> plain = platform::xpeccyConfigCandidates();
		CHECK(!plain.empty());
		// Exactly what xcore/config.cpp builds, which is $HOME and not
		// XDG_CONFIG_HOME. Copying the emulator's own mistake is the point:
		// the server has to look where the GUI actually wrote.
		CHECK_EQ(plain.front(),
			 std::string("/test-home") + kSep + ".config" + kSep +
			 "samstyle" + kSep + "xpeccy");

		setEnv("XSP_XPECCY_CONF", "/forced/path");
		std::vector<std::string> forced = platform::xpeccyConfigCandidates();
		CHECK(forced.size() == plain.size() + 1);
		CHECK_EQ(forced.front(), std::string("/forced/path"));

		// XDG_CONFIG_HOME is consulted as well, but after the emulator's own
		// location, so a machine that has both keeps working the way the GUI
		// left it.
		setEnv("XSP_XPECCY_CONF", "");
		setEnv("XDG_CONFIG_HOME", "/xdg");
		std::vector<std::string> withXdg = platform::xpeccyConfigCandidates();
		CHECK(withXdg.size() == plain.size() + 1);
		CHECK_EQ(withXdg.front(), plain.front());
		bool sawXdg = false;
		for (const std::string& c : withXdg)
			if (c.find("/xdg") == 0) sawXdg = true;
		CHECK(sawXdg);

		setEnv("XSP_XPECCY_CONF", savedForce.empty() ? nullptr : savedForce.c_str());
		setEnv("HOME", savedHome.empty() ? nullptr : savedHome.c_str());
		setEnv("XDG_CONFIG_HOME", savedXdg.empty() ? nullptr : savedXdg.c_str());
	}

	// setBinaryStdio does nothing outside Windows and must not be a crash
	// anywhere; there is nothing else to observe about it from in here.
	platform::setBinaryStdio();
	CHECK(true);
}
