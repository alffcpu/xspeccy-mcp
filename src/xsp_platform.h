// The handful of things that differ between Linux, macOS and Windows, kept in
// one place so the rest of the server can stay platform-blind.
#pragma once

#include <string>
#include <vector>

namespace xsp {
namespace platform {

std::string homeDir();		// $HOME, or %USERPROFILE% on Windows
std::string tempDir();		// $TMPDIR/$TEMP/$TMP, or /tmp

bool isDir(const std::string& path);
bool isFile(const std::string& path);
bool makeDir(const std::string& path);		// single level, ok if it exists

std::string join(const std::string& a, const std::string& b);

// Where Xpeccy keeps its configuration, most likely first. On Unix this is
// $HOME/.config/samstyle/xpeccy, exactly as xcore/config.cpp builds it; on
// Windows the GUI puts it next to its own .exe, so there is nothing to guess -
// pass --config or set XSP_XPECCY_CONF.
std::vector<std::string> xpeccyConfigCandidates();

// Windows translates \n on the way out, which would corrupt the line-delimited
// JSON-RPC stream. No-op elsewhere.
void setBinaryStdio();

// This process, for naming scratch files. Two servers running out of the same
// temporary directory otherwise write over each other's work, and the one that
// loses does not find out.
int processId();

} // namespace platform
} // namespace xsp
