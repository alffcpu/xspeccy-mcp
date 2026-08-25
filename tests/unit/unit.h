#pragma once
//
// A test harness small enough to read in one sitting. There is no framework
// here on purpose: this project vendors exactly one dependency (nlohmann/json)
// and a second one bought nothing that forty lines do not.
//
// Every check prints only when it fails, so a passing run is one line per suite
// and the failures are the whole output.
//
#include <cstdio>
#include <string>

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
inline std::string text(double v) { return std::to_string(v); }
inline std::string text(const std::string& v) { return "\"" + v + "\""; }
inline std::string text(const char* v) { return std::string("\"") + (v ? v : "(null)") + "\""; }

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

void test_blend();
void test_labels();
void test_listing();
void test_png();
