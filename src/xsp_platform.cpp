#include "xsp_platform.h"

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
	#include <direct.h>
	#include <fcntl.h>
	#include <io.h>
	#include <process.h>
	#include <windows.h>
	#ifndef S_ISDIR
		#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
	#endif
	#ifndef S_ISREG
		#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
	#endif
#else
	#include <dirent.h>
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

std::vector<std::string> listDir(const std::string& path) {
	std::vector<std::string> res;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(join(path, "*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return res;
	do {
		if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
			res.push_back(fd.cFileName);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR* d = opendir(path.c_str());
	if (!d) return res;
	while (dirent* e = readdir(d))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			res.push_back(e->d_name);
	closedir(d);
#endif
	return res;
}

std::vector<std::string> xpeccyConfigCandidates() {
	std::vector<std::string> res;
	std::string forced = env("XSP_XPECCY_CONF");
	if (!forced.empty()) res.push_back(forced);

	std::string xdg = env("XDG_CONFIG_HOME");

	// xpeccy-plus keeps its own directory ($XDG_CONFIG_HOME/xpeccy-plus, or
	// ~/.config without it) so the two emulators can sit side by side; the old
	// samstyle path stays as a fallback for configurations this server was
	// already running against
	if (xdg.empty()) {
		res.push_back(join(join(homeDir(), ".config"), "xpeccy-plus"));
		res.push_back(join(join(join(homeDir(), ".config"), "samstyle"), "xpeccy"));
	} else {
		res.push_back(join(xdg, "xpeccy-plus"));
		res.push_back(join(join(xdg, "samstyle"), "xpeccy"));
		res.push_back(join(join(join(homeDir(), ".config"), "samstyle"), "xpeccy"));
	}

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
