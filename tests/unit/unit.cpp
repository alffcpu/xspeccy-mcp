#include "unit.h"
#include "xsp_platform.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#ifdef _WIN32
	#include <direct.h>
	#include <process.h>
	#define rmdir _rmdir
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

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


// ---------------------------------------------------------------- TempDir

TempDir::TempDir(const char* tag) {
	// The system temporary directory, asked for the same way the server asks:
	// the point is to work on a build machine whose TMPDIR is not /tmp, which
	// is every macOS machine.
	std::string base = xsp::platform::tempDir();
	static int counter = 0;
	char name[64];
	std::snprintf(name, sizeof(name), "xsp-unit-%s-%ld-%d", tag,
		      (long)getpid(), counter++);
	m_path = xsp::platform::join(base, name);
	if (!xsp::platform::makeDir(m_path))
		std::fprintf(stderr, "  WARN cannot create %s\n", m_path.c_str());
}

TempDir::~TempDir() {
	for (size_t i = m_files.size(); i > 0; i--) std::remove(m_files[i - 1].c_str());
	for (size_t i = m_dirs.size(); i > 0; i--) rmdir(m_dirs[i - 1].c_str());
	rmdir(m_path.c_str());
}

std::string TempDir::dir(const std::string& relative) {
	std::string path = m_path;
	size_t start = 0;
	while (start <= relative.size()) {
		size_t slash = relative.find('/', start);
		std::string part = relative.substr(start, slash == std::string::npos
						   ? std::string::npos : slash - start);
		if (!part.empty()) {
			path = xsp::platform::join(path, part);
			xsp::platform::makeDir(path);
			m_dirs.push_back(path);
		}
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	return path;
}

std::string TempDir::file(const std::string& relative, const std::string& contents) {
	const size_t slash = relative.find_last_of('/');
	std::string path = slash == std::string::npos
		? xsp::platform::join(m_path, relative)
		: xsp::platform::join(dir(relative.substr(0, slash)), relative.substr(slash + 1));
	// Binary, so the fixtures are the exact bytes written here on every
	// platform. A config file that grows CRLF on Windows is a different test.
	std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
	out << contents;
	out.close();
	m_files.push_back(path);
	return path;
}

std::string TempDir::reserve(const std::string& relative) {
	const size_t slash = relative.find_last_of('/');
	std::string path = slash == std::string::npos
		? xsp::platform::join(m_path, relative)
		: xsp::platform::join(dir(relative.substr(0, slash)), relative.substr(slash + 1));
	m_files.push_back(path);
	return path;
}

std::string readWholeFile(const std::string& path) {
	std::ifstream in(path.c_str(), std::ios::binary);
	std::ostringstream buf;
	buf << in.rdbuf();
	return buf.str();
}

} // namespace unit
