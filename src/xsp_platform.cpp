#include "xsp_platform.h"

#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
	#include <direct.h>
	#include <fcntl.h>
	#include <io.h>
	#include <process.h>
	#ifndef S_ISDIR
		#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
	#endif
	#ifndef S_ISREG
		#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
	#endif
#else
	#include <unistd.h>
#endif

namespace xsp {
namespace platform {

static std::string env(const char* name) {
	const char* v = getenv(name);
	return (v && *v) ? std::string(v) : std::string();
}

std::string homeDir() {
	std::string h = env("HOME");
	if (!h.empty()) return h;
#ifdef _WIN32
	h = env("USERPROFILE");
	if (!h.empty()) return h;
	std::string drive = env("HOMEDRIVE"), path = env("HOMEPATH");
	if (!path.empty()) return drive + path;
#endif
	return ".";
}

std::string tempDir() {
	for (const char* v : {"TMPDIR", "TEMP", "TMP"}) {
		std::string t = env(v);
		if (!t.empty()) return t;
	}
#ifdef _WIN32
	return "C:\\Windows\\Temp";
#else
	return "/tmp";
#endif
}

bool isDir(const std::string& path) {
	struct stat st;
	return !stat(path.c_str(), &st) && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& path) {
	struct stat st;
	return !stat(path.c_str(), &st) && S_ISREG(st.st_mode);
}

bool makeDir(const std::string& path) {
	if (isDir(path)) return true;
#ifdef _WIN32
	return _mkdir(path.c_str()) == 0;
#else
	return mkdir(path.c_str(), 0700) == 0;
#endif
}

std::string join(const std::string& a, const std::string& b) {
	if (a.empty()) return b;
	if (b.empty()) return a;
	char last = a[a.size() - 1];
	if (last == '/' || last == '\\') return a + b;
#ifdef _WIN32
	return a + "\\" + b;
#else
	return a + "/" + b;
#endif
}

std::vector<std::string> xpeccyConfigCandidates() {
	std::vector<std::string> res;
	std::string forced = env("XSP_XPECCY_CONF");
	if (!forced.empty()) res.push_back(forced);

	// what xcore/config.cpp:conf_init() builds on Unix - note it uses $HOME
	// directly and ignores XDG_CONFIG_HOME, so we must too
	res.push_back(join(join(join(homeDir(), ".config"), "samstyle"), "xpeccy"));

	std::string xdg = env("XDG_CONFIG_HOME");
	if (!xdg.empty()) res.push_back(join(join(xdg, "samstyle"), "xpeccy"));

#ifdef _WIN32
	std::string appdata = env("APPDATA");
	if (!appdata.empty()) res.push_back(join(join(appdata, "samstyle"), "xpeccy"));
#endif
	return res;
}

void setBinaryStdio() {
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stdin), _O_BINARY);
#endif
}

int processId() {
#ifdef _WIN32
	return (int)_getpid();
#else
	return (int)getpid();
#endif
}

} // namespace platform
} // namespace xsp
