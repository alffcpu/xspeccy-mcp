// MCP server for the Xpeccy ZX Spectrum core: JSON-RPC 2.0 over stdio, one
// message per line, logs on stderr.
//
// This file is the entry point and nothing else. The protocol lives in
// xsp_server.cpp, the argument rules in xsp_args.cpp, and the fifty-odd tools in
// src/tools/, one file per subject.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "tools/common.h"
#include "xsp_platform.h"
#include "xsp_server.h"

namespace server = xsp::server;
namespace tools = xsp::tools;

static const char* const kUsage =
	"usage: xspeccy-mcp [--config <xpeccy config dir>] [--settings <file>]\n"
	"       xspeccy-mcp --version\n";

int main(int argc, char** argv) {
	std::string configDir;
	std::string settingsPath;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--version") {
			printf("%s %s\n", server::kServerName, server::kServerVersion);
			printf("Xpeccy %s", server::kXpeccyVersion);
			if (strcmp(server::kXpeccyVersion, server::kXpeccyPinned) != 0)
				printf(" (pinned: %s - this build does not match it)", server::kXpeccyPinned);
			printf("\n");
			return 0;
		}
		if (a == "--help") {
			printf("%s", kUsage);
			return 0;
		}
		if (a == "--config") {
			if (i + 1 >= argc) {
				fprintf(stderr, "xspeccy-mcp: --config needs a directory\n");
				return 1;
			}
			configDir = argv[++i];
		}
		else if (a.compare(0, 9, "--config=") == 0) configDir = a.substr(9);
		else if (a == "--settings") {
			if (i + 1 >= argc) {
				fprintf(stderr, "xspeccy-mcp: --settings needs a file\n");
				return 1;
			}
			settingsPath = argv[++i];
		}
		else if (a.compare(0, 11, "--settings=") == 0) settingsPath = a.substr(11);
		else {
			// Anything else is a mistake worth stopping for. Ignoring it means
			// a mistyped --setings starts a server with different settings than
			// the caller asked for and says nothing about it, which is the same
			// failure as the one below and just as hard to see afterwards.
			fprintf(stderr, "xspeccy-mcp: unknown argument '%s'\n%s", a.c_str(), kUsage);
			return 1;
		}
	}

	// A --config nobody can read is an error, not a quiet fall back. Without
	// this the search order takes over and the server comes up on whatever
	// configuration the host happens to have - a different machine from the one
	// that was asked for, reported as if it were the right one.
	if (!configDir.empty() && !xsp::platform::isDir(configDir)) {
		fprintf(stderr, "xspeccy-mcp: --config: no such directory: %s\n", configDir.c_str());
		return 1;
	}
	if (!settingsPath.empty() && !xsp::platform::isFile(settingsPath)) {
		fprintf(stderr, "xspeccy-mcp: --settings: no such file: %s\n", settingsPath.c_str());
		return 1;
	}

	xsp::platform::setBinaryStdio();	// Windows would otherwise rewrite \n

	std::string err;
	if (!server::machine().init(err, configDir)) {
		fprintf(stderr, "xspeccy-mcp: %s\n", err.c_str());
		return 1;
	}
	fprintf(stderr, "xspeccy-mcp: %s\n", server::machine().env().note.c_str());

	// Our own settings, after the emulator's configuration and before anything
	// runs, so the first frame is already the machine the caller asked for.
	{
		auto rep = xsp::settings::loadFiles(server::machine(), server::machine().env().confDir, settingsPath);
		for (const std::string& f : rep.files)
			fprintf(stderr, "xspeccy-mcp: settings from %s\n", f.c_str());
		for (const std::string& a : rep.applied)
			fprintf(stderr, "xspeccy-mcp:   %s\n", a.c_str());
		// Loudly, on the way in: a settings file that is silently half-ignored
		// is worse than none, because the machine is then not what it says.
		for (const std::string& p : rep.problems)
			fprintf(stderr, "xspeccy-mcp: settings: %s\n", p.c_str());
	}

	server::machine().runFrames(100);			// reach the prompt before the first request

	tools::registerAll();
	fprintf(stderr, "xspeccy-mcp: %s, Xpeccy %s%s\n", server::kServerVersion, server::kXpeccyVersion,
		strcmp(server::kXpeccyVersion, server::kXpeccyPinned) == 0 ? "" : " (NOT the pinned release)");
	fprintf(stderr, "xspeccy-mcp: %zu tools ready\n", server::tools().size());

	std::ios::sync_with_stdio(false);
	server::serve(std::cin, std::cout);
	return 0;
}
