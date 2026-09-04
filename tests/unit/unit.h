#pragma once
//
// A test harness small enough to read in one sitting. There is no framework
// here on purpose: this project vendors exactly one dependency (nlohmann/json)
// and a second one bought nothing that two macros and a self-cleaning temporary
// directory do not.
//
// Every check prints only when it fails, so a passing run is one line per suite
// and the failures are the whole output.
//
#include <cstdio>
#include <string>
#include <vector>

namespace unit {

extern int g_checks;
extern int g_failures;

void begin(const char* name);
void fail(const char* expr, const char* file, int line, const std::string& detail);
int summary();

inline std::string text(int v) { return std::to_string(v); }
inline std::string text(long v) { return std::to_string(v); }
inline std::string text(unsigned v) { return std::to_string(v); }
inline std::string text(unsigned long v) { return std::to_string(v); }
inline std::string text(long long v) { return std::to_string(v); }
inline std::string text(unsigned long long v) { return std::to_string(v); }
inline std::string text(double v) { return std::to_string(v); }
inline std::string text(const std::string& v) { return "\"" + v + "\""; }
inline std::string text(const char* v) { return std::string("\"") + (v ? v : "(null)") + "\""; }

// A directory that cleans up after itself. Several of these suites need real
// files on disk - a configuration tree to read, a settings file to rewrite, a
// GIF to write and read back - and a test that leaves litter in the system
// temporary directory is a test people stop running.
//
// Only what was created through here is removed, in reverse order, so there is
// no recursive delete in the test harness and no way for a wrong path to take
// anything else with it.
class TempDir {
public:
	explicit TempDir(const char* tag);
	~TempDir();

	// Deletes what it created, so a copy would delete it twice and the second
	// one would be deleting whatever had taken the name since.
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;

	const std::string& path() const { return m_path; }

	std::string dir(const std::string& relative);			// mkdir -p, one level at a time
	std::string file(const std::string& relative, const std::string& contents);
	std::string reserve(const std::string& relative);		// a path to be written by the code under test

private:
	std::string m_path;
	std::vector<std::string> m_files;
	std::vector<std::string> m_dirs;
};

std::string readWholeFile(const std::string& path);

} // namespace unit

#define CHECK(cond)                                                            \
	do {                                                                   \
		++::unit::g_checks;                                            \
		if (!(cond)) ::unit::fail(#cond, __FILE__, __LINE__, "");      \
	} while (0)

// Prints both sides when it fails. A bare CHECK(a == b) says only that they
// differ, and the number that came out is usually the whole diagnosis.
#define CHECK_EQ(got, want)                                                    \
	do {                                                                   \
		++::unit::g_checks;                                            \
		const auto _g = (got);                                         \
		const auto _w = (want);                                        \
		if (!(_g == _w))                                               \
			::unit::fail(#got " == " #want, __FILE__, __LINE__,    \
				     "got " + ::unit::text(_g) +               \
				     ", want " + ::unit::text(_w));            \
	} while (0)

void test_args();
void test_audio();
void test_blend();
void test_config();
void test_gif();
void test_keyboard();
void test_labels();
void test_listing();
void test_machine();
void test_platform();
void test_png();
void test_server();
void test_settings();
