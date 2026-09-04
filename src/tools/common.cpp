#include "tools/common.h"

namespace xsp {
namespace tools {

Timing timing(Computer* c) {
	Timing t;
	if (c->nsPerTick > 0) {
		t.tPerFrame = c->vid->nsPerFrame / c->nsPerTick;
		t.tPerLine = c->vid->nsPerLine / c->nsPerTick;
	}
	if (c->cpuFrq > 0) {
		t.nsPerT = 1000.0 / c->cpuFrq;			// cpuFrq is in MHz
		t.nsPerFrame = t.nsPerT * t.tPerFrame;
		if (t.tPerFrame > 0) t.fps = c->cpuFrq * 1e6 / t.tPerFrame;
	}
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
	if (model == "ZX48K" || model == "Scorpion") return 69888;		// 312 lines x 224 T
	if (model == "Spectrum +2" || model == "Spectrum +3") return 70908;	// 311 x 228
	if (model == "Pentagon" || model == "Pentagon1024SL") return 71680;	// 320 x 224
	return 0;
}

std::string geometryWarning(const Timing& tm) {
	int want = canonicalFrameT(mach().model());
	if (!want || !tm.tPerFrame || want == tm.tPerFrame) return "";
	char buf[512];
	snprintf(buf, sizeof(buf),
		 "geometry '%s' gives %d T per frame, but a %s runs %d - timings here are "
		 "%+.1f%% out. The raster comes from the geometry, not from the model: name one "
		 "whose height matches (machine_config {\"geometry\": ...}), or read these numbers "
		 "as the geometry's rather than the machine's.",
		 mach().layout().c_str(), tm.tPerFrame, mach().model().c_str(), want,
		 100.0 * (tm.tPerFrame - want) / (double)want);
	return buf;
}

// The disk controller the machine is wired with. Worth reporting: a ZX machine without one
// cannot boot TR-DOS at all, and the failure looks like a bad ROM rather than a missing drive.
const char* diskInterfaceName(int type) {
	switch (type) {
		case DIF_BDI: return "Beta Disk (TR-DOS)";
		case DIF_P3DOS: return "+3 uPD765";
		case DIF_PC: return "PC";
		case DIF_SMK512: return "SMK512";
		default: return "none";
	}
}

json machineStateJson() {
	Computer* c = mach().comp();
	Timing tm = timing(c);
	json pages = json::array();
	for (int a = 0; a < 0x10000; a += 0x4000) {
		xAdr xa = mem_get_xadr(c->mem, a);
		pages.push_back({
			{"cpu_address", hex16(a)},
			{"type", xa.type == MEM_ROM ? "rom" : (xa.type == MEM_RAM ? "ram" : "other")},
			{"bank", xa.bank}
		});
	}
	json out = {
		{"model", mach().model()},
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
		{"screen", {{"width", c->vid->vsze.x}, {"height", c->vid->vsze.y}}},
		{"disk_interface", diskInterfaceName(c->dif->type)},
		{"memory_map", pages}
	};
	std::string warn = geometryWarning(tm);
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
}

} // namespace tools
} // namespace xsp
