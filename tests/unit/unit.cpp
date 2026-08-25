#include "unit.h"

namespace unit {

int g_checks = 0;
int g_failures = 0;
static const char* g_suite = "";
static int g_suiteFailures = 0;

void begin(const char* name) {
	g_suite = name;
	g_suiteFailures = g_failures;
}

void fail(const char* expr, const char* file, int line, const std::string& detail) {
	++g_failures;
	std::fprintf(stderr, "  FAIL %s  %s:%d\n        %s\n", g_suite, file, line, expr);
	if (!detail.empty()) std::fprintf(stderr, "        %s\n", detail.c_str());
}

int summary() {
	std::printf("%d checks, %d failed\n", g_checks, g_failures);
	(void)g_suiteFailures;
	return g_failures == 0 ? 0 : 1;
}

} // namespace unit
