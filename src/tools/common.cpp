#include "tools/common.h"

#include <set>

namespace xsp {
namespace tools {

Timing timing(Computer* c) {
	Timing t;
	if (c->nsPerTick <= 0) return t;
	// the quotient is exact T-states arrived at through rounded ns:
	// 228 T/line at 281.94 ns/T is 227.9 back - truncate and a ZX128
	// reports 227 T/line against every table ever printed
	t.tPerFrame = (int)llround(c->vid->nsPerFrame / c->nsPerTick);
	t.tPerLine = (int)llround(c->vid->nsPerLine / c->nsPerTick);
	// nsPerTick IS the effective tick - comp_update_timings already divided
	// the base clock by turbo and hardware multipliers - so the wall-clock
	// figures come from it. Deriving them from cpuFrq quoted the base clock
	// for a machine actually running twice as fast.
	t.nsPerT = c->nsPerTick;
	t.nsPerFrame = t.nsPerT * t.tPerFrame;
	if (t.tPerFrame > 0) t.fps = 1e9 / t.nsPerFrame;
	return t;
}

// The raster belongs to the geometry, not to the model. Xpeccy keeps it in the
// profile (xcore/profiles.cpp:prfSetLayout) and its built-in "default" layout is
// 448x320 (xcore/config.cpp:104), which is Pentagon's. Naming a model therefore
// does not change the frame length: ask for a ZX48K on the default geometry and
// every timing tool answers 71680 T where the machine's own raster is 69888 - a
// 2.5% error that looks exactly like a real reading.
//
// We report it rather than override it, because the machine you get should stay
// the machine the GUI would have booted. 0 means "we do not claim to know this
// one's raster", and nothing is said.
static int canonicalFrameT(const std::string& model) {
	// model() is the core's hardware name; the frame length is the geometry's
	// (res/layouts.conf: lines x dots where the dots count in half-T units).
	// A definition already pinned its own geometry through applyLayout, so
	// this only adjudicates machines addressed by hardware name alone.
	if (model == "ZX48" || model == "ZX48K" || model == "Scorpion" ||
	    model == "ATM2" || model == "ALF" || model == "Profi")
		return 69888;			// 312-line geometries: ULA.48/Scorpion/ATM2/Profi5
	if (model == "ZX128" || model == "Plus2A" || model == "Plus3" ||
	    model == "Spectrum +2" || model == "Spectrum +2A" || model == "Spectrum +3")
		return 70908;			// ULA.128/Plus3: 311 x 228
	if (model == "ScorpionTP")
		return 70784;			// ULA.ScorpionTP: 316 x 224
	if (model == "Pentagon" || model == "P1024" || model == "Pentagon1024SL" ||
	    model == "Baseconf" || model == "PentEvo" || model == "Profi3" ||
	    model == "Phoenix" || model == "TSConf")
		return 71680;			// 320-line geometries: Pentagon/Evo/TSConf/Profi
	return 0;
}

std::string geometryWarning(const Timing& tm, bool brief) {
	int canonical = canonicalFrameT(mach().model());
	if (!canonical || !tm.tPerFrame) return "";
	// the canonical figure is a base-clock T-state count, and tPerFrame is
	// counted in the effective tick - under turbo the same raster needs
	// twice the ticks, which is correct timing, not a wrong geometry
	Computer* c = mach().comp();
	int want = canonical;
	if (c->nsPerTick > 0 && c->cpuFrq > 0) {
		const double scale = (1000.0 / c->cpuFrq) / c->nsPerTick;
		want = (int)llround(canonical * scale);
	}
	if (want == tm.tPerFrame) return "";
	char buf[512];
	if (brief)
		snprintf(buf, sizeof(buf),
			 "geometry '%s' gives %d T per frame, a %s runs %d: timings %+.1f%% out",
			 mach().layout().c_str(), tm.tPerFrame, mach().model().c_str(), want,
			 100.0 * (tm.tPerFrame - want) / (double)want);
	else
		snprintf(buf, sizeof(buf),
			 "geometry '%s' gives %d T per frame, but a %s runs %d - timings here are "
			 "%+.1f%% out. The raster comes from the geometry, not from the model: name one "
			 "whose height matches (machine_config {\"geometry\": ...}), or read these numbers "
			 "as the geometry's rather than the machine's.",
			 mach().layout().c_str(), tm.tPerFrame, mach().model().c_str(), want,
			 100.0 * (tm.tPerFrame - want) / (double)want);
	return buf;
}

std::string geometryNotice(const Timing& tm) {
	const std::string full = geometryWarning(tm);
	if (full.empty()) return full;
	// The explanation is worth its full wording once; after that the facts
	// alone. The short form still names both figures and the error, because
	// "said earlier" is no use to a reader who never saw it - a subagent on
	// the same server, or a conversation whose start has been summarized.
	static std::set<std::string> said;
	if (said.insert(full).second) return full;
	return geometryWarning(tm, true);
}

// The disk controller the machine is wired with. Worth reporting: a ZX machine without one
// cannot boot TR-DOS at all, and the failure looks like a bad ROM rather than a missing drive.
const char* diskInterfaceName(int type) {
	switch (type) {
		case DIF_BDI: return "Beta Disk (TR-DOS)";
		case DIF_P3DOS: return "+3 uPD765";
		default: return "none";
	}
}

json machineStateJson() {
	Computer* c = mach().comp();
	Timing tm = timing(c);
	json pages = json::array();
	for (int a = 0; a < 0x10000; a += 0x4000) {
		xAdr xa = mem_get_xadr(c->mem, a);
		// xa.bank is the core's page number in units of its page size (256
		// bytes on a 16-bit bus), not the 16K bank a bank:offset address or a
		// symbol file names. The absolute address divided by 16K is that bank
		// whatever the page size - the same sum Machine::bankAt() does.
		const int bank16 = (xa.type == MEM_ROM || xa.type == MEM_RAM) ? xa.abs / 0x4000
									  : xa.bank;
		pages.push_back({
			{"cpu_address", hex16(a)},
			{"type", xa.type == MEM_ROM ? "rom" : (xa.type == MEM_RAM ? "ram" : "other")},
			{"bank", bank16}
		});
	}
	json out = {
		{"model", mach().model()},
		{"machine", mach().machineDefId()},	// "" when no definition applied
		{"memory_kb", mach().memoryKb()},
		{"romset", mach().romset()},
		{"geometry", mach().layout()},
		{"cpu", c->cpu->core ? c->cpu->core->name : "?"},
		{"cpu_frq_mhz", c->cpuFrq},
		{"fps_real", tm.fps},			// clock / T-states per frame
		{"t_states_per_frame", tm.tPerFrame},
		{"pc", cpu_get_pc(c->cpu)},
		{"pc_hex", hex16(cpu_get_pc(c->cpu))},
		{"t_states_frame", c->frmtCount},
		{"screen", {{"width", c->vid->vsze.x * video::frameScale(c)}, {"height", c->vid->vsze.y}}},
		{"disk_interface", diskInterfaceName(c->dif->type)},
		{"memory_map", pages}
	};
	std::string warn = geometryNotice(tm);
	if (!warn.empty()) out["warning"] = warn;
	return out;
}

void registerAll() {
	// The order is the order tools/list reports them in, and it is the order a
	// reader would want: what the machine is, how to run it, how to look inside
	// it, and only then the things that produce files.
	registerMachineTools();
	registerExecutionTools();
	registerMemoryTools();
	registerDebugTools();
	registerVideoTools();
	registerSourceTools();
	registerTimingTools();
	registerAudioTools();
	registerInputTools();
	registerMediaTools();
	registerDiskTools();
}

} // namespace tools
} // namespace xsp
