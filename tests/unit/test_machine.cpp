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

#include <cmath>
#include <memory>
#include <cstdlib>
#include <string>

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

	// Machine is over four megabytes of Computer; it does not go on the stack.
	std::unique_ptr<Machine> owned(new Machine);
	Machine& m = *owned;
	std::string err;
	CHECK(m.init(err, tmp.dir("conf")));
	CHECK(err.empty());
	CHECK(m.comp() != nullptr);
	if (!m.comp()) return;

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
		CHECK(r.ns > 0);
		CHECK_EQ(r.reason, std::string("frames"));

		// A step is one instruction and the PC has to have moved.
		const int before = cpu_get_pc(m.comp()->cpu);
		RunResult one = m.step(1);
		CHECK_EQ(one.instructions, 1);
		CHECK(one.pc != before || before == 0);

		// A budget that cannot be met stops for that reason and says so, rather
		// than running until somebody kills the process.
		RunResult capped = m.run(50, -1, -1);
		CHECK_EQ(capped.instructions, 50);
		CHECK_EQ(capped.reason, std::string("budget"));
	}
}
