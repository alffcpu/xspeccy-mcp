// Everything the tool implementations share.
//
// The tools are grouped one file per subject - the machine, execution, memory,
// the picture, sound - and this is the header all of them start from. It is a
// deliberately wide include: each group needs a different part of the server,
// and one prelude that names all of them is easier to keep honest than eleven
// lists that drift.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

#include "xsp_args.h"
#include "xsp_audio.h"
#include "xsp_blend.h"
#include "xsp_config.h"
#include "xsp_keyboard.h"
#include "xsp_labels.h"
#include "xsp_listing.h"
#include "xsp_machine.h"
#include "xsp_platform.h"
#include "xsp_record.h"
#include "xsp_server.h"
#include "xsp_settings.h"
#include "xsp_video.h"

extern "C" {
#include "filetypes/filetypes.h"
}

namespace xsp {
namespace tools {

// Two using-declarations rather than a using-directive, and only inside
// xsp::tools: every tool file calls tool() and the arg readers on nearly every
// line, and qualifying them there would be noise. Nothing outside this namespace
// sees either name.
using server::tool;
using namespace args;	// argNum, argAddr, hex16, kAddrArg and the rest

// The machine every tool acts on.
inline Machine& mach() { return server::machine(); }

// The frame as the core actually times it, derived rather than nominal.
//
// The core keeps time in whole nanoseconds: nsPerTick is 1000/cpuFrq truncated
// and then forced even (3.5 MHz gives 284 rather than 285.714). T-states per
// frame and per line are exact, so the honest frame rate comes from those and
// the clock. The core used to carry a nominal `fps` beside them, set to 50 for
// every ZX machine whatever its geometry; upstream deleted it, and `fps_real`
// is what it should have been.
struct Timing {
	int tPerFrame = 0;		// exact, from the core's own dot timing
	int tPerLine = 0;
	double nsPerT = 0.0;		// real, from the CPU clock
	double nsPerFrame = 0.0;
	double fps = 0.0;
};

Timing timing(Computer* c);

// Said whenever the geometry's raster is not the one the named model runs, which
// is a 2.5% error that otherwise reads as a real measurement.
std::string geometryWarning(const Timing& tm);

const char* diskInterfaceName(int type);
json machineStateJson();

// Registration, one call per subject. main() calls registerAll().
void registerAll();
void registerMachineTools();
void registerExecutionTools();
void registerMemoryTools();
void registerDebugTools();
void registerVideoTools();
void registerSourceTools();
void registerTimingTools();
void registerAudioTools();
void registerInputTools();
void registerMediaTools();

} // namespace tools
} // namespace xsp
