// Every emulator setting this server can change, in one table.
//
// Before this existed a setting could be in three places at once - a value
// written into the source, a line in the config parser, an argument on some
// tool - and nothing compared them. That is how a server ends up quietly
// running a machine nobody asked for: an audit of every setting found the
// reset bank booting 48 BASIC while the emulator's own profile said TR-DOS, and
// the contention pattern set to a different group of banks than the profile.
//
// So there is one row per setting and one place that knows how to read it, write
// it and describe it. A new setting is a row; there is no second list to forget.
//
// What is deliberately NOT here: anything fixed at build time (see the same
// document, group E) and anything that only means something to a window - full
// screen, shaders, key maps. A headless server has no opinion on those, and
// listing them would suggest it does.
#pragma once

#include <string>
#include <vector>

namespace xsp {

class Machine;

namespace settings {

enum class Type { Bool, Int, Enum, Double };

// What it takes for a change to mean anything. The honest answer to "can this be
// changed while running", which is the first thing anyone asks.
enum class Scope {
	Live,		// the next frame or the next instruction sees it
	NeedsReset,	// only a reset makes it real
	NeedsRestart	// read once at startup
};

// Consequences a caller has to know about, because they invalidate work already
// done rather than just changing what happens next.
enum Flags {
	None = 0,
	TimingBaseline = 1 << 0,	// every frame_cost/profile/beam_log measured
					// before this is no longer comparable
	DropsHistory = 1 << 1		// blended captures lose their frames
};

struct Setting {
	const char* name;		// "timing.contPattern"
	Type type;
	const char* const* enumNames;	// null-terminated, Enum only
	double lo, hi;			// Int/Double bounds; refused outside, never clamped
	double def;			// what the core builds when nobody says otherwise
	Scope scope;
	unsigned flags;
	const char* xpeccyKey;		// the key in Xpeccy's own config, or nullptr
	const char* affects;		// one line for the report
	double (*get)(Machine&);
	void (*set)(Machine&, double);
};

const std::vector<Setting>& table();
const Setting* find(const std::string& name);

// Where the value in force came from. Reported per setting, because "why is the
// machine like this" is unanswerable without it.
enum class Source { Default, XpeccyProfile, File, Session };
Source sourceOf(const std::string& name);
void markSource(const std::string& name, Source src);
const char* sourceName(Source s);

// Text <-> number for one setting: enums by name, bools as true/false/yes/no,
// numbers as themselves. Returns false and fills err rather than guessing.
bool parseValue(const Setting& s, const std::string& text, double& out, std::string& err);
std::string formatValue(const Setting& s, double v);

// Applies `value` after checking it against the row. Does not touch the source
// map; the caller says where the value came from.
bool apply(Machine& m, const Setting& s, double value, std::string& err);

// ---- the settings file ------------------------------------------------
//
// KEY = VALUE with [section] headers, the same dull format as VERSIONS, so a
// person can edit it and xsp::parseIni-style code can read it. A section makes
// the prefix: [timing] contPattern -> timing.contPattern.
//
// Search order, later winning: beside Xpeccy's own configuration, then
// ./xspeccy-mcp.conf in the working directory. The second is the one that
// matters for a project: settings live in the project's own repository, so
// every session starts from the same machine and changing one is a commit.
struct LoadReport {
	std::vector<std::string> files;		// read, in order
	std::vector<std::string> applied;	// "name = value"
	std::vector<std::string> problems;	// unknown key, bad value, out of range
};
LoadReport loadFiles(Machine& m, const std::string& xpeccyConfigDir,
		     const std::string& explicitPath);

// Writes one setting into the file, preserving everything else in it, and
// creating it if it does not exist. `path` empty picks the first search
// location that exists, else ./xspeccy-mcp.conf.
bool save(const std::string& path, const std::string& name, const std::string& value,
	  std::string& usedPath, std::string& err);

std::string defaultSavePath(const std::string& xpeccyConfigDir);

} // namespace settings
} // namespace xsp
