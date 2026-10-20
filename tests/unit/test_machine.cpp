// A real machine, built the way the server builds one, and the settings table
// applied to it.
//
// Heavier than the rest of this suite, and worth it: the settings table is the
// server's claim that every emulator setting it can change lives in one place,
// and half of that claim is the get/set pair on each row actually reaching the
// core. Nothing but a running machine can check that, and until now nothing did:
// the end-to-end suite sets a handful of settings and the rest were taken on
// trust.
#include "unit.h"
#include "xsp_machine.h"
#include "xsp_platform.h"
#include "xsp_settings.h"
#include "xsp_video.h"

#include <climits>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <cstdlib>
#include <string>

extern "C" {
#include "cpu/Z80/z80.h"	// regIM, flgIFF1, Z80_INT: an interrupt set up by hand
}

using namespace xsp;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
	_putenv_s(name, value ? value : "");
#else
	if (value && *value) setenv(name, value, 1);
	else unsetenv(name);
#endif
}

std::string getEnv(const char* name) {
	const char* v = getenv(name);
	return v ? v : "";
}

// The same isolation the configuration suite uses: the machine under test must
// be the one this file describes, not the one the developer's Xpeccy is set to.
class NoHostConfig {
public:
	explicit NoHostConfig(const std::string& emptyHome)
		: m_home(getEnv("HOME")), m_xdg(getEnv("XDG_CONFIG_HOME")),
		  m_forced(getEnv("XSP_XPECCY_CONF")) {
		setEnv("HOME", emptyHome.c_str());
		setEnv("XDG_CONFIG_HOME", nullptr);
		setEnv("XSP_XPECCY_CONF", nullptr);
	}
	~NoHostConfig() {
		setEnv("HOME", m_home.c_str());
		setEnv("XDG_CONFIG_HOME", m_xdg.c_str());
		setEnv("XSP_XPECCY_CONF", m_forced.c_str());
	}
private:
	std::string m_home, m_xdg, m_forced;
};

} // namespace

void test_machine() {
	unit::begin("machine");

	unit::TempDir tmp("machine");
	NoHostConfig isolated(tmp.path());

	// A user file over a shipped stem is a patch, not a replacement; and a
	// user machine that names roms nobody has still has to boot.
	tmp.file("conf/machines/zx48.conf",
		 "[machine]\n"
		 "memory = 96\n");
	tmp.file("conf/machines/norom.conf",
		 "[machine]\n"
		 "hw = ZX48\n"
		 "[rom]\n"
		 "rom0 = absent.rom\n");
	// memory=0 is not "none": mac_ram_size() substitutes the default, 64K
	tmp.file("conf/machines/mem0.conf",
		 "[machine]\n"
		 "hw = ZX48\n"
		 "memory = 0\n");
	// a machine that names a font and a GS rom; the files exist so the
	// resources really load - the point is they are gone on the next machine
	tmp.file("conf/machines/resourced.conf",
		 "[machine]\n"
		 "hw = ZX48\n"
		 "[rom]\n"
		 "gs = test.gsr\n"
		 "font = test.fnt\n");
	tmp.file("conf/roms/test.gsr", std::string(1024, '\x55'));
	tmp.file("conf/roms/test.fnt", std::string(2048, '\x3c'));

	// Machine is over four megabytes of Computer; it does not go on the stack.
	std::unique_ptr<Machine> owned(new Machine);
	Machine& m = *owned;
	std::string err;
	CHECK(m.init(err, tmp.dir("conf")));
	CHECK(err.empty());
	CHECK(m.comp() != nullptr);
	if (!m.comp()) return;

	// ---- the patch layered over the shipped definition ----------------------
	//
	// zx48's file says contmem=yes; the patch changes only memory. And the
	// romset a machine cannot load must not leave it booting on FF - the
	// configured set, or the built-in, takes over instead.
	{
		const MachineDef* z = m.env().findMachine("zx48");
		CHECK(z != nullptr);
		if (z) {
			CHECK_EQ(z->memoryKb, 96);		// the patch's own key
			CHECK_EQ(z->contmem, 1);		// shipped, untouched
			CHECK(z->roms.size() >= 1);		// shipped rom0 still there
		}
		std::string e;
		CHECK(m.setModel("norom", e));
		CHECK_EQ(m.machineDefId(), std::string("norom"));
		CHECK(m.romset() != std::string("machine:norom"));
		CHECK(!m.romset().empty());	// fell back to a usable romset
	}

	// ---- what it came up as -------------------------------------------------
	{
		CHECK(!m.model().empty());
		CHECK(!m.romset().empty());
		CHECK(!m.layout().empty());
		CHECK(m.memoryKb() > 0);
		// A machine that booted has a clock and a raster; either at zero means
		// every timing tool would divide by it.
		CHECK(m.comp()->cpuFrq > 0);
		CHECK(m.comp()->nsPerTick > 0);
		CHECK(m.comp()->vid->nsPerFrame > 0);
	}

	// ---- changing the hardware ----------------------------------------------
	{
		const std::vector<std::string> models = m.models();
		CHECK(models.size() > 3);

		// The list is what the core knows, and a few of those are emulated but
		// cannot be run headlessly - the PC-9801's IO handlers print to stdout,
		// which here is the JSON-RPC stream. The contract is not "every name
		// works"; it is that a name either works or is refused with a reason,
		// and never half-applied. That is what list_models says and what this
		// checks.
		int usable = 0, refused = 0;
		for (const std::string& name : models) {
			std::string e;
			if (m.setModel(name, e)) {
				CHECK_EQ(m.model(), name);
				CHECK(e.empty());
				usable++;
			} else {
				CHECK(!e.empty());
				CHECK(e.find(name) != std::string::npos);
				// Refused means unchanged: the machine is still whatever it
				// was, not the one that could not be built.
				CHECK(m.model() != name);
				refused++;
			}
		}
		CHECK(usable > 10);
		CHECK(usable + refused == (int)models.size());

		std::string e;
		CHECK(!m.setModel("Amstrad CPC", e));	// not in the core at all
		CHECK(e.find("unknown machine") != std::string::npos);
		CHECK(m.model() != "Amstrad CPC");

		// RAM is clamped to what the hardware has, and the clamp is visible:
		// memoryKb() reports what was actually set rather than what was asked
		// for, which is what machine_state then hands back to the caller.
		CHECK(m.setModel("ZX48K", e));
		CHECK(m.setMemory(48, e));
		// 64, not 48. The core allocates RAM in powers of two, so a 48K
		// Spectrum gets a 64K block and reports it; machine_state says
		// memory_kb: 64 for a ZX48K and always has. Written down here because
		// it looks like a bug and is not one, and because changing it would
		// mean either reallocating the core's memory or reporting a size that
		// is not the one in use.
		CHECK_EQ(m.memoryKb(), 64);

		CHECK(m.setModel("Pentagon", e));
		CHECK(m.setMemory(128, e));
		CHECK_EQ(m.memoryKb(), 128);
		// A Pentagon has no 48K mode, so asking for one gets a size it does
		// have. Accepted rather than refused, and the number that comes back
		// says which.
		CHECK(m.setMemory(48, e));
		CHECK(m.memoryKb() != 48);
		CHECK(m.memoryKb() >= 128);
		CHECK(m.setMemory(128, e));

		// Sizes that are not sizes at all are refused before the multiply,
		// which used to overflow into a value inside the valid range.
		CHECK(!m.setMemory(0, e));
		CHECK(!m.setMemory(-1, e));
		CHECK(!m.setMemory(1 << 22, e));
		CHECK_EQ(m.memoryKb(), 128);

		CHECK(m.setLayout("default", e));
		CHECK_EQ(m.layout(), std::string("default"));
		CHECK(!m.setLayout("no such geometry", e));
		CHECK(!e.empty());

		CHECK(!m.setRomset("no such romset", e));
	}

	// ---- reading and writing memory -----------------------------------------
	{
		m.reset();
		m.writeByte(0x8000, 0x3e);
		m.writeByte(0x8001, 0x05);
		CHECK_EQ(m.readByte(0x8000), 0x3e);
		CHECK_EQ(m.readByte(0x8001), 0x05);

		// ROM is not writable, and a write into it must be a no-op rather than
		// a change that lasts until the next reset.
		const int romByte = m.readByte(0x0000);
		m.writeByte(0x0000, (romByte ^ 0xff) & 0xff);
		CHECK_EQ(m.readByte(0x0000), romByte);

		std::vector<unsigned char> pattern = {0x3e, 0x05};
		CHECK_EQ(m.findBytes(pattern, 0x8000, 0xffff), 0x8000);
		CHECK_EQ(m.findBytes(pattern, 0x8002, 0xffff), -1);
	}

	// ---- the settings table against a real machine ---------------------------
	//
	// Every row read, written and read back. A row whose get and set reach
	// different fields - or whose set reaches nothing at all, which is what
	// happens when a pointer in the chain is null on this model - passes every
	// other test in this project and silently does nothing.
	{
		using namespace xsp::settings;
		for (const Setting& s : table()) {
			std::string e;
			const double before = s.get(m);

			// The default is always applicable: it is what the core builds.
			CHECK(apply(m, s, s.def, e));
			CHECK(e.empty());

			// A second value, chosen inside the row's own bounds, has to come
			// back as itself.
			double other = s.def;
			if (s.type == Type::Enum) {
				int n = 0;
				while (s.enumNames[n]) n++;
				other = (s.def + 1 < n) ? s.def + 1 : 0;
			} else if (s.type == Type::Bool) {
				other = s.def != 0 ? 0 : 1;
			} else if (s.type == Type::Int) {
				other = (s.def < s.hi) ? s.def + 1 : s.lo;
			} else {
				other = (s.def + s.hi) / 2.0;
			}
			if (other != s.def) {
				CHECK(apply(m, s, other, e));
				const double back = s.get(m);
				if (s.type == Type::Double)
					CHECK(std::fabs(back - other) < 1e-6);
				else
					CHECK_EQ(back, other);
			}
			apply(m, s, before, e);
		}
	}

	// ---- the setters that reach the core through a call, not a field ------
	//
	// On xpeccy-plus some settings are only fields: the code reads them where
	// the behaviour happens (contendedIo, ulaPlus, borderStep). Others carry
	// a hook the field alone cannot install, and a setter that writes just
	// the field leaves a setting that reads "on" and does nothing - the
	// regression this used to be. The table goes through the core's own
	// setters now, so the check is on the hook, not the flag.
	{
		std::string e;
		Computer* c = m.comp();
		CHECK(m.setModel("ZX48", e));

		// comp_set_cont() is the only thing that wires cpu->flgCONT and
		// cpu->xcont; writing flgCNTM made the report lie and the bus fast.
		const xsp::settings::Setting* contmem = xsp::settings::find("timing.contendedMemory");
		CHECK(contmem != nullptr);
		CHECK(xsp::settings::apply(m, *contmem, 0, e));
		CHECK_EQ(c->flgCNTM, 0);
		CHECK_EQ(c->cpu->flgCONT, 0);
		CHECK(xsp::settings::apply(m, *contmem, 1, e));
		CHECK_EQ(c->cpu->flgCONT, 1);
		CHECK(c->cpu->xcont != nullptr);

		// ts_set_frq() recalculates tickFx; a bare frq write left the chip
		// ticking at the old rate while reporting the new one.
		const xsp::settings::Setting* cfrq = xsp::settings::find("sound.chipFrequency");
		CHECK(cfrq != nullptr);
		aymChip* a = c->ts->chipA;
		CHECK(a != nullptr);
		CHECK(xsp::settings::apply(m, *cfrq, 1.0, e));
		const long long was = a->tickFx;
		CHECK(xsp::settings::apply(m, *cfrq, 2.0, e));
		CHECK(a->tickFx != was);	// recalculated, not just relabelled
		CHECK_EQ(a->tickFx, (long long)(2.0 * 2 * 4294967.296));
		CHECK(xsp::settings::apply(m, *cfrq, cfrq->def, e));

		// chip_set_type() routes the callbacks; a bare type write relabelled
		// the chip but left it wired - or silenced - the old way.
		const xsp::settings::Setting* ctype = xsp::settings::find("sound.chipType");
		CHECK(ctype != nullptr);
		CHECK(xsp::settings::apply(m, *ctype, SND_NONE, e));
		CHECK_EQ(a->type, SND_NONE);
		CHECK(a->vol != ay_vol);		// routed to the silent stubs
		CHECK(xsp::settings::apply(m, *ctype, SND_AY, e));
		CHECK(a->vol == ay_vol);

		// The type moves the clock: a YM2203 runs at twice the AY's, so a
		// type change that kept frq dropped the SSG an octave.
		CHECK(xsp::settings::apply(m, *ctype, SND_YM2203, e));
		CHECK(std::fabs(a->frq - 2 * c->ts->frq) < 1e-9);
		CHECK(xsp::settings::apply(m, *ctype, SND_AY, e));
		CHECK(std::fabs(a->frq - c->ts->frq) < 1e-9);

		// One setting is every chip on the board. A TurboSound retuned or
		// retyped through chipA alone played its two halves apart.
		c->ts->type = TS_NEDOPC;
		chip_set_type(c->ts->chipB, SND_AY);
		aymChip* b = c->ts->chipB;
		CHECK(xsp::settings::apply(m, *cfrq, 2.0, e));
		CHECK(std::fabs(b->frq - 2.0) < 1e-9);
		CHECK_EQ(b->tickFx, a->tickFx);
		CHECK(xsp::settings::apply(m, *ctype, SND_YM, e));
		CHECK_EQ(b->type, SND_YM);
		CHECK_EQ(c->ts->chipC->type, SND_NONE);	// the slot the board does not have
		const xsp::settings::Setting* stereo = xsp::settings::find("sound.chipStereo");
		CHECK(stereo != nullptr);
		if (stereo) {
			CHECK(xsp::settings::apply(m, *stereo, AY_ACB, e));
			CHECK_EQ(b->stereo, AY_ACB);
			CHECK(xsp::settings::apply(m, *stereo, stereo->def, e));
		}
		CHECK(xsp::settings::apply(m, *ctype, SND_AY, e));
		CHECK(xsp::settings::apply(m, *cfrq, cfrq->def, e));
		c->ts->type = TS_NONE;
		chip_set_type(c->ts->chipB, SND_NONE);

		// The pattern also picks the screen drawer: with contention the ULA
		// fetches early (VID_ULA_SCR), without it it does not (VID_NORMAL).
		// Writing conttype alone left the old drawer up until a reset.
		const xsp::settings::Setting* cpat = xsp::settings::find("timing.contPattern");
		CHECK(cpat != nullptr);
		CHECK(xsp::settings::apply(m, *cpat, CONT_NONE, e));
		CHECK_EQ(c->vid->vmode, VID_NORMAL);
		CHECK(xsp::settings::apply(m, *cpat, CONT_PATA, e));
		CHECK_EQ(c->vid->vmode, VID_ULA_SCR);
	}

	// Out of range is refused and the machine keeps the value it had. Clamping
	// would make the file say one thing and the machine be another.
	{
		using namespace xsp::settings;
		const Setting* gamma = find("video.gamma");
		CHECK(gamma != nullptr);
		if (gamma) {
			std::string e;
			CHECK(apply(m, *gamma, 2.2, e));
			CHECK(!apply(m, *gamma, 99.0, e));
			CHECK(e.find("outside") != std::string::npos);
			CHECK(std::fabs(gamma->get(m) - 2.2) < 1e-6);
			CHECK(!apply(m, *gamma, -1.0, e));
		}

		const Setting* pattern = find("timing.contPattern");
		CHECK(pattern != nullptr);
		if (pattern) {
			std::string e;
			CHECK(!apply(m, *pattern, 99, e));
			CHECK(e.find("no such option") != std::string::npos);
			CHECK(!apply(m, *pattern, -1, e));
		}
	}

	// ---- the settings file ---------------------------------------------------
	//
	// Read on the way in, and everything it could not use has to be reported.
	// A file that is silently half-applied is worse than none, because the
	// machine is then not what the file says it is.
	{
		using namespace xsp::settings;
		const std::string path = tmp.file("good.conf",
			"# the machine this project measures against\n"
			"[video]\n"
			"gamma = 2.4\n"
			"blend = 2\n"
			"[timing]\n"
			"earlyTiming = yes\n");
		LoadReport rep = loadFiles(m, "", path);
		CHECK_EQ((int)rep.problems.size(), 0);
		CHECK(rep.applied.size() >= 3);
		bool sawFile = false;
		for (const std::string& f : rep.files) if (f == path) sawFile = true;
		CHECK(sawFile);

		const Setting* gamma = find("video.gamma");
		if (gamma) CHECK(std::fabs(gamma->get(m) - 2.4) < 1e-6);
		// And it is recorded as having come from a file, because "why is the
		// machine like this" is the question the source map exists to answer.
		CHECK(sourceOf("video.gamma") == Source::File);
	}

	// Every kind of bad line is named rather than dropped: a key nobody knows,
	// a value of the wrong shape, and a number outside the row's range.
	{
		using namespace xsp::settings;
		const std::string path = tmp.file("bad.conf",
			"[video]\n"
			"gamma = sideways\n"
			"gama = 2.2\n"
			"blend = 9999\n"
			"[nonsense]\n"
			"whatever = 1\n");
		LoadReport rep = loadFiles(m, "", path);
		CHECK((int)rep.problems.size() >= 4);
		std::string all;
		for (const std::string& p : rep.problems) all += p + "\n";
		CHECK(all.find("sideways") != std::string::npos);
		CHECK(all.find("gama") != std::string::npos);
		CHECK(all.find("9999") != std::string::npos);
		CHECK(all.find("whatever") != std::string::npos);
	}

	// A --settings path that is not there is a problem to report, not a silent
	// start on the defaults.
	{
		using namespace xsp::settings;
		LoadReport rep = loadFiles(m, "", platform::join(tmp.path(), "absent.conf"));
		CHECK((int)rep.problems.size() >= 1);
		CHECK(rep.problems.back().find("cannot be read") != std::string::npos);
	}

	// ---- running -------------------------------------------------------------
	{
		m.reset();
		RunResult r = m.runFrames(2);
		CHECK_EQ(r.frames, 2);
		CHECK(r.instructions > 0);
		CHECK(r.t > 0);
		CHECK_EQ(r.reason, std::string("frames"));

		// nanoseconds are accumulated per executed instruction, not derived
		// from the final tick: zero here would mean every compExec paid nothing
		CHECK(r.ns > 0);

		// A step is one instruction and the PC has to have moved.
		const int before = cpu_get_pc(m.comp()->cpu);
		RunResult one = m.step(1);
		CHECK_EQ(one.instructions, 1);
		CHECK(one.ns > 0);
		CHECK(one.pc != before || before == 0);
		// The step's T is the cpu's own tick count, so it is the same number
		// instrTiming measures in the sandbox CPU - either outcome of a
		// conditional, the only figure for anything else. Divided-back
		// nanoseconds would not come out to the exact integer.
		InstrTiming it = m.instrTiming(before);
		CHECK(one.t == it.taken || one.t == it.not_taken);

		// A budget that cannot be met stops for that reason and says so, rather
		// than running until somebody kills the process.
		RunResult capped = m.run(50, -1, -1);
		CHECK_EQ(capped.instructions, 50);
		CHECK_EQ(capped.reason, std::string("budget"));
	}

	// ---- machine definitions decide whole machines --------------------------
	//
	// A definition is complete - what its file does not carry falls to the
	// defaults, not to whatever the previous machine had. The ZX48's file
	// asks for contended memory and the Pentagon's says nothing about it,
	// because the real Pentagon had none.
	{
		std::string e;
		CHECK(m.setModel("ZX48", e));
		CHECK_EQ(m.comp()->flgCNTM, 1);
		CHECK(m.setModel("Pentagon", e));
		CHECK_EQ(m.comp()->flgCNTM, 0);	// was: stayed 1 from the ZX48
		CHECK(m.setModel("ZX48", e));
		CHECK_EQ(m.comp()->flgCNTM, 1);	// and back, both directions

		// Several stems can share one hardware: zx128 and zxplus2 are both
		// ZX128 boards, but the +2 carries different roms and turns
		// earlyTiming off. The stem that was asked for is the definition
		// that must apply - looking the def up again by hardware name used
		// to land on zx128 either way.
		CHECK(m.setModel("zx128", e));
		CHECK_EQ(m.machineDefId(), std::string("zx128"));
		CHECK_EQ(m.comp()->vid->ula->early, 1);
		CHECK(m.setModel("zxplus2", e));
		CHECK_EQ(m.machineDefId(), std::string("zxplus2"));
		CHECK_EQ(m.comp()->vid->ula->early, 0);	// the +2's own key

		// The core's own alias table knows TSLab means TSConf; it lands on
		// the same definition asking for TSConf would.
		CHECK(m.setModel("TSLab", e));
		CHECK_EQ(m.model(), std::string("TSConf"));
		CHECK_EQ(m.machineDefId(), std::string("evo-tsconf"));

		// Half-dot video modes put two real pixels in one dot slot, so the
		// dot column count is not the frame's pixel width; full-dot modes
		// duplicate one pixel there and the two numbers match. The double-
		// density modes reach the same half-dot writer through vidDrawByteDD,
		// not through a *TEXT name - the mode enum, not the name, decides.
		m.comp()->vid->vmode = VID_TSL_TEXT;
		CHECK_EQ(video::pixelScale(m.comp()), 2);
		m.comp()->vid->vmode = VID_ATM_TEXT;
		CHECK_EQ(video::pixelScale(m.comp()), 2);
		m.comp()->vid->vmode = VID_EVO_TEXT;
		CHECK_EQ(video::pixelScale(m.comp()), 2);
		m.comp()->vid->vmode = VID_ATM_HWM;
		CHECK_EQ(video::pixelScale(m.comp()), 2);
		m.comp()->vid->vmode = VID_PRF_MC;
		CHECK_EQ(video::pixelScale(m.comp()), 2);
		m.comp()->vid->vmode = VID_PRF_MONO;
		CHECK_EQ(video::pixelScale(m.comp()), 2);
		m.comp()->vid->vmode = VID_NORMAL;
		CHECK_EQ(video::pixelScale(m.comp()), 1);
		m.comp()->vid->vmode = VID_UNKNOWN;	// -1: not a mode at all
		CHECK_EQ(video::pixelScale(m.comp()), 1);

		// memory=0 in a definition is mac_ram_size's "use the default",
		// not "no RAM" - the machine comes up with 64K
		CHECK(m.setModel("mem0", e));
		CHECK_EQ(m.memoryKb(), 64);

		// a Profi v3 takes 768K - the one fitted size that is not a power
		// of two - where a lesser board clamps to what it has
		CHECK(m.setModel("profi3", e));
		CHECK(m.setMemory(768, e));
		CHECK_EQ(m.memoryKb(), 768);

		CHECK(m.setModel("Pentagon", e));
	}

	// ---- resources a machine leaves behind ----------------------------------
	//
	// The GS firmware and the text font belong to the machine that named
	// them. A definition that names neither must not keep the previous
	// machine's copies (upstream clears both, and a missing file is the
	// same as no name, not a reason to keep them).
	{
		std::string e;
		CHECK(m.setModel("resourced", e));
		CHECK(m.comp()->vid->font.data != nullptr);
		CHECK(m.comp()->vid->font.size > 0);
		CHECK_EQ(m.comp()->gs->mem->romSize, 1024);
		CHECK_EQ(m.comp()->gs->mem->romData[0], 0x55);

		// ZX48 names no font and no GS rom: both go
		CHECK(m.setModel("ZX48", e));
		CHECK(m.comp()->vid->font.data == nullptr);
		CHECK_EQ(m.comp()->gs->mem->romSize, 0);
		CHECK_EQ(m.comp()->gs->mem->romData[0], 0xff);	// cleared, not kept
	}

	// ---- the T-state counter wrapping ---------------------------------------
	//
	// tickCount is the core's signed int, and it wraps past INT_MAX about
	// ten minutes of emulation in (2^31 / 3.5e6 = 613 s). The delta across a single
	// instruction has to come out of that wrap a small positive number -
	// computed as an unsigned difference - not a two-billion-step regression.
	{
		m.reset();
		const int saved = m.comp()->tickCount;
		m.comp()->tickCount = INT_MAX - 8;
		RunResult r = m.step(3);
		CHECK(r.t > 0);
		CHECK(r.t < 300);
		m.comp()->tickCount = saved;
	}

	// ---- a fetch break does not count the instruction it stopped ------------
	//
	// compExec returns before the opcode runs when a fetch break fires: the
	// instruction at the break address never executed, so it must not end up
	// in the instruction count, the profile or the coverage.
	{
		m.reset();
		m.writeByte(0x8000, 0xf3);		// DI
		m.writeByte(0x8001, 0x00);		// NOP behind a fetch break
		cpu_set_pc(m.comp()->cpu, 0x8000);
		m.setBreakAddress(0x8001, MEM_BRK_FETCH);
		RunResult r = m.run(100, -1, -1);
		CHECK_EQ(r.reason, std::string("breakpoint"));
		CHECK_EQ(r.instructions, 1);	// DI ran; the NOP under the break did not
		CHECK_EQ(r.pc, 0x8001);
		m.clearBreaks();
	}

	auto poke = [&](int adr, std::initializer_list<int> bytes) {
		for (int b : bytes) m.writeByte(adr++, b);
	};

	// ---- clearing finds the cell that was armed -----------------------------
	//
	// A cell break lives in the core's map at the index the RAM mask gave it.
	// Bank 7 offset 5 on a Pentagon is cell $1C005; on a 64K machine the same
	// address masks to $C005. Clearing by re-masking with whatever mask is
	// current then clears $C005 and leaves $1C005 armed - and back on the
	// Pentagon, the first other breakpoint wakes it up.
	{
		std::string e;
		CHECK(m.setModel("Pentagon", e));
		Computer* c = m.comp();
		CHECK(m.setBreakBank(7, 0x0005, MEM_BRK_FETCH, e));
		CHECK_EQ(c->brkRamMap[0x1c005] & 0x0f, MEM_BRK_FETCH);
		CHECK(m.setModel("alf", e));
		CHECK(c->mem->ramMask < 0x1c005);	// the case: a mask that cannot reach it
		m.clearBreaks();
		CHECK(m.setModel("Pentagon", e));
		CHECK_EQ(c->brkRamMap[0x1c005] & 0x0f, 0);
		CHECK_EQ(c->brkRamMap[0x0c005] & 0x0f, 0);

		// and the code there runs past it once something else arms the maps
		m.reset();
		poke(0x8000, {0x01, 0xfd, 0x7f,		// ld bc,$7ffd
			      0x3e, 0x17,		// ld a,$17: bank 7 at $C000
			      0xed, 0x79,		// out (c),a
			      0xc3, 0x05, 0xc0});	// jp $c005
		unsigned char* bank7 = c->mem->ramData + 7 * 0x4000;
		bank7[5] = 0x00; bank7[6] = 0x18; bank7[7] = 0xfd;	// nop / jr $-1
		cpu_set_pc(c->cpu, 0x8000);
		m.setBreakAddress(0x9000, MEM_BRK_FETCH);	// never reached; raises flgBRKMEM
		RunResult r = m.run(200, -1, -1);
		CHECK_EQ(r.reason, std::string("budget"));
		m.clearBreaks();

		// A bank the machine does not have is refused, not masked into one it
		// does: 15 on a 128K machine would be bank 7.
		CHECK(!m.setBreakBank(15, 0x0005, MEM_BRK_FETCH, e));
		CHECK(e.find("no RAM bank 15") != std::string::npos);
		CHECK(!m.setBreakBank(8, 0, MEM_BRK_FETCH, e));
		CHECK(!m.setBreakBank(2, 0x4000, MEM_BRK_FETCH, e));
		CHECK_EQ(c->brkRamMap[0x1c005] & 0x0f, 0);
		CHECK_EQ(c->brkRamMap[0x08000] & 0x0f, 0);
		CHECK_EQ(m.readBankByte(0, 0x4000), -1);	// the next bank, not a wrap
		CHECK_EQ(m.readBankByte(0, -1), -1);
		CHECK_EQ(m.readBankByte(8, 0), -1);
	}

	// ---- a port break on $FE is the ULA's port ------------------------------
	//
	// The core looks a port up by all 16 bits, and `out ($FE),a` puts A on the
	// top half. A low-byte port is armed under every high byte; a full 16-bit
	// one stays exact.
	{
		std::string e;
		CHECK(m.setModel("Pentagon", e));
		Computer* c = m.comp();
		m.reset();
		poke(0x8000, {0x3e, 0x07,		// ld a,7
			      0xd3, 0xfe,		// out ($fe),a: port $07FE
			      0x18, 0xfe});		// jr $
		cpu_set_pc(c->cpu, 0x8000);
		m.setIoBreak(0xfe, MEM_BRK_WR);
		RunResult r = m.run(100, -1, -1);
		CHECK_EQ(r.reason, std::string("breakpoint"));
		CHECK_EQ(r.brk_type, (int)BRK_IOPORT);
		CHECK_EQ(r.brk_addr, 0x07fe);
		m.clearBreaks();
		CHECK_EQ(c->brkIOMap[0x07fe], 0);
		CHECK_EQ(c->brkIOMap[0xfffe], 0);

		m.setIoBreak(0x7ffd, MEM_BRK_WR);
		CHECK_EQ(c->brkIOMap[0x7ffd], MEM_BRK_WR);
		CHECK_EQ(c->brkIOMap[0x3ffd], 0);
		CHECK_EQ(c->brkIOMap[0x00fd], 0);
		m.clearBreaks();
	}

	// ---- an accepted interrupt is not an instruction ------------------------
	//
	// z80_exec() either takes the interrupt or runs the opcode at PC. A step
	// that took it ran nothing at PC: that instruction runs after the handler
	// returns. Counting it there put the interrupted PC in the profile, the
	// coverage, the trace and the beam log twice.
	{
		std::string e;
		CHECK(m.setModel("Pentagon", e));
		Computer* c = m.comp();
		auto pendInt = [&]() {
			m.reset();
			poke(0x8000, {0x18, 0xfe});	// jr $
			cpu_set_pc(c->cpu, 0x8000);
			c->cpu->regIM = 1;
			c->cpu->flgIFF1 = 1;
			c->cpu->flgIFF2 = 1;
			c->cpu->flgNOINT = 0;
			c->cpu->flgACK = 1;
			c->cpu->inten |= Z80_INT;
			c->cpu->intrq |= Z80_INT;
		};
		Profile& p = m.profile();
		p.ranges.clear();
		p.reset();
		p.on = true;
		m.coverage().reset();
		m.coverage().on = true;
		m.trace().reset(16);
		m.trace().on = true;
		m.beamLog().reset(16);
		m.beamLog().clearWatch();
		m.beamLog().addWatch(0x8000);
		m.beamLog().on = true;

		pendInt();
		RunResult r = m.step(1);
		CHECK_EQ(r.pc, 0x0038);			// a step still lands on the handler
		CHECK_EQ(r.instructions, 0);
		CHECK_EQ(r.interrupts, 1);
		CHECK_EQ(r.t, 13);			// IM 1, not halted
		CHECK_EQ(p.instructions, 0);
		CHECK_EQ(p.page[0x80], 0);
		CHECK_EQ(p.interrupts, 1);
		CHECK_EQ(p.interruptT, 13);
		CHECK_EQ(p.t, 13);
		CHECK_EQ(p.otherT, 0);
		CHECK_EQ(m.coverage().instructions, 0);
		CHECK_EQ(m.coverage().map[0x8000], 0);
		CHECK(m.trace().dump(16).empty());
		CHECK_EQ(m.beamLog().total, 0);

		// the run loop: the budget counts the handler's first instruction,
		// not the acceptance
		p.reset();
		m.coverage().reset();
		pendInt();
		r = m.run(1, -1, -1);
		CHECK_EQ(r.instructions, 1);
		CHECK_EQ(r.interrupts, 1);
		CHECK_EQ(p.instructions, 1);
		CHECK_EQ(p.page[0x80], 0);
		CHECK_EQ(p.page[0x00], 1);
		CHECK_EQ(m.coverage().map[0x8000], 0);
		CHECK(m.coverage().map[0x0038] & 1);
		CHECK_EQ(m.beamLog().total, 0);

		// and frame_cost, which keeps its own instruction count
		pendInt();
		FrameCost fc = m.frameCost(0x0038, 10);
		CHECK(fc.complete);
		CHECK_EQ(fc.instructions, 0);
		CHECK_EQ(fc.work_t, 13);

		p.on = false;
		m.coverage().on = false;
		m.trace().on = false;
		m.beamLog().on = false;
		m.beamLog().clearWatch();
	}

	// ---- a fetch break does not log the beam for what it stopped -----------
	//
	// The beam stamp is taken before the step, but the fetch break returns
	// before the opcode runs; the resumed run executes that PC and logs it.
	// Logging both put the same execution in the log twice.
	{
		Computer* c = m.comp();
		m.reset();
		poke(0x8000, {0xf3, 0x00, 0x18, 0xfe});	// di / nop / jr $
		cpu_set_pc(c->cpu, 0x8000);
		m.beamLog().reset(16);
		m.beamLog().clearWatch();
		m.beamLog().addWatch(0x8001);
		m.beamLog().on = true;
		m.setBreakAddress(0x8001, MEM_BRK_FETCH);
		RunResult r = m.run(100, -1, -1);
		CHECK_EQ(r.reason, std::string("breakpoint"));
		CHECK_EQ(m.beamLog().total, 0);
		r = m.run(1, -1, -1);			// steps off the break, runs the nop
		CHECK_EQ(r.pc, 0x8002);
		CHECK_EQ(m.beamLog().total, 1);
		m.clearBreaks();
		m.beamLog().on = false;
		m.beamLog().clearWatch();
	}

	// ---- coverage marks the instruction, not the distance jumped ------------
	//
	// A taken `jr c,$+3` moves PC by 3, which used to read as three bytes of
	// straight-line code - the byte it jumped over came out as executed.
	{
		Computer* c = m.comp();
		m.reset();
		poke(0x8000, {0x37,			// scf
			      0x38, 0x01,		// jr c,$+3
			      0x00,			// skipped
			      0x00,			// nop
			      0xed, 0xb0});		// ldir: repeats, PC moves back
		cpu_set_pc(c->cpu, 0x8000);
		cpu_set_reg(c->cpu, "BC", 2);
		cpu_set_reg(c->cpu, "HL", 0x9000);
		cpu_set_reg(c->cpu, "DE", 0x9100);
		m.coverage().reset();
		m.coverage().on = true;
		m.step(4);				// scf, jr, nop, first ldir pass
		const unsigned char* map = m.coverage().map;
		CHECK(map[0x8000] & 1);
		CHECK(map[0x8001] & 1);
		CHECK(map[0x8002] & 1);
		CHECK_EQ(map[0x8003], 0);
		CHECK(map[0x8004] & 1);
		CHECK(map[0x8005] & 1);
		CHECK(map[0x8006] & 1);			// ldir's second byte, though PC went back
		m.coverage().on = false;
	}

	// ---- a romset that does not load leaves the ROM alone -------------------
	//
	// The load used to fill the ROM with FF and set the new size before any
	// file was read, so a romset with a missing file reported an error while
	// the machine went on running FF under the old romset's name.
	{
		std::string e;
		CHECK(m.setModel("Pentagon", e));
		m.reset();
		Computer* c = m.comp();
		const int mask = c->mem->romMask;
		const std::string named = m.romset();
		const int b0 = m.readByte(0x0000), b1 = m.readByte(0x2000);
		CHECK(b0 != 0xff);
		m.env().romsets.push_back(Romset{"Broken", {RomFile{"absent.rom"}}});
		CHECK(!m.setRomset("Broken", e));
		CHECK(e.find("absent.rom") != std::string::npos);
		CHECK_EQ(m.romset(), named);
		CHECK_EQ(c->mem->romMask, mask);
		CHECK_EQ(m.readByte(0x0000), b0);
		CHECK_EQ(m.readByte(0x2000), b1);

		// An 8K rom is padded to 16K with FF, as upstream's mac_load_rom
		// does - not mirrored by an 8K mask into $2000..$3FFF.
		tmp.file("conf/roms/short8k.rom", std::string(0x2000, '\x11'));
		m.env().romsets.push_back(Romset{"Short", {RomFile{"short8k.rom"}}});
		CHECK(m.setRomset("Short", e));
		m.reset();
		CHECK_EQ(c->mem->romMask, 0x3fff);
		CHECK_EQ(m.readByte(0x0000), 0x11);
		CHECK_EQ(m.readByte(0x1fff), 0x11);
		CHECK_EQ(m.readByte(0x2000), 0xff);
		m.env().romsets.pop_back();
		m.env().romsets.pop_back();
		CHECK(m.setModel("Pentagon", e));
	}

	// ---- what a machine could not load goes with it -------------------------
	//
	// The notes in config_source used to only grow: switch to a machine whose
	// GS rom is missing and back, and the note about it stayed for the rest of
	// the session as if it described the machine in use.
	{
		std::string e;
		CHECK(m.setModel("Pentagon", e));
		const std::string clean = m.env().note;
		MachineDef d;
		d.id = "nogs";
		d.hw = "ZX48";
		d.gsRom = "missing.gsr";
		m.env().machines.push_back(d);
		CHECK(m.setModel("nogs", e));
		CHECK(m.env().note.find("missing.gsr") != std::string::npos);
		CHECK(m.setModel("Pentagon", e));
		CHECK(m.env().note.find("missing.gsr") == std::string::npos);
		CHECK_EQ(m.env().note, clean);
		m.env().machines.pop_back();
	}
}
