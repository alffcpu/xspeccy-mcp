#include "xsp_machine.h"

#include "xsp_blend.h"
#include "xsp_builtin.h"
#include "xsp_platform.h"
#include "xsp_video.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

extern "C" {
#include "hardware/hardware.h"
#include "sound/ayym.h"			// aymChip for machine-definition psg.* keys
#include "sound/soundrive.h"		// SDRV_* is stored straight into sdrv->type
#include "cpu/Z80/z80.h"		// Z80_INT/Z80_NMI: which interrupt a step takes
// The table of machines the core knows. Upstream stopped declaring it in the header and
// left the definition where it was, so the GUI declares it for itself in setupwin.cpp and
// this is the same line. Entries with a NULL core are menu separators.
extern tabHwItem tabHwPtr[];
}

namespace xsp {

// ---------------------------------------------------------------- helpers

bool parseNumber(const std::string& raw, int& out) {
	std::string s = trim(raw);
	if (s.empty()) return false;
	int sign = 1;
	size_t i = 0;
	if (s[0] == '-') { sign = -1; i = 1; }
	int base = 10;
	if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) { base = 16; i += 2; }
	else if (s[i] == '$' || s[i] == '#') { base = 16; i += 1; }
	else if (s[i] == '%') { base = 2; i += 1; }
	if (i >= s.size()) return false;
	char* end = nullptr;
	long v = strtol(s.c_str() + i, &end, base);
	if (!end || *end != '\0') return false;
	out = (int)(sign * v);
	return true;
}

static int toPowerOfTwo(int v) {
	int r = 1;
	while (r < v) r <<= 1;
	return r;
}

// ---------------------------------------------------------------- profiling

void Profile::reset() {
	instructions = 0;
	t = 0;
	ns = 0;
	frames = 0;
	otherInstructions = 0;
	otherT = 0;
	otherNs = 0;
	interrupts = 0;
	interruptT = 0;
	interruptNs = 0;
	prevPc = -1;
	memset(page, 0, sizeof(page));
	stack.clear();
	for (auto& r : ranges) {
		r.instructions = 0;
		r.t = 0;
		r.ns = 0;
		r.calls = 0;
		r.entries = 0;
		r.startHits = 0;
		r.inside = false;
		r.inclusiveT = 0;
		r.inclusiveNs = 0;
		r.inclusiveInstructions = 0;
	}
}

void Coverage::reset() {
	memset(map, 0, sizeof(map));
	instructions = 0;
}

void Coverage::mark(int from, int to) {
	for (int a = from; a <= to; a++) map[a & 0xffff] |= 1;
}

void Trace::reset(size_t cap) {
	if (cap < 1) cap = 1;
	if (cap > 1 << 20) cap = 1 << 20;
	capacity = cap;
	pcs.assign(cap, -1);
	pos = 0;
	wrapped = false;
}

void Trace::add(int pc) {
	if (pcs.empty()) reset(capacity);
	pcs[pos] = pc;
	pos = (pos + 1) % pcs.size();
	if (pos == 0) wrapped = true;
}

std::vector<int> Trace::dump(size_t count) const {
	std::vector<int> res;
	if (pcs.empty()) return res;
	size_t have = wrapped ? pcs.size() : pos;
	if (count > have) count = have;
	for (size_t i = 0; i < count; i++) {
		size_t idx = (pos + pcs.size() - count + i) % pcs.size();
		res.push_back(pcs[idx]);
	}
	return res;
}

void BeamLog::reset(size_t cap) {
	if (cap < 1) cap = 1;
	if (cap > 1 << 20) cap = 1 << 20;
	capacity = cap;
	events.assign(cap, Event());
	pos = 0;
	wrapped = false;
	total = 0;
}

void BeamLog::clearWatch() {
	memset(watch, 0, sizeof(watch));
}

void BeamLog::add(const Event& e) {
	if (events.empty()) reset(capacity);
	events[pos] = e;
	pos = (pos + 1) % events.size();
	if (pos == 0) wrapped = true;
	total++;
}

std::vector<BeamLog::Event> BeamLog::dump(size_t count) const {
	std::vector<Event> res;
	if (events.empty()) return res;
	size_t have = wrapped ? events.size() : pos;
	if (count > have) count = have;
	for (size_t i = 0; i < count; i++) {
		size_t idx = (pos + events.size() - count + i) % events.size();
		res.push_back(events[idx]);
	}
	return res;
}

std::vector<int> BeamLog::watched() const {
	std::vector<int> res;
	for (int a = 0; a < 0x10000; a++)
		if (watch[a]) res.push_back(a);
	return res;
}

// The beam as it stands right now. Read before the instruction runs, so the
// stamp is "when this instruction began" rather than where it left the beam,
// which is the number a raster deadline is actually measured against.
BeamLog::Event Machine::beamEvent(int pc) const {
	BeamLog::Event e;
	e.pc = pc;
	e.frame = m_comp->vid->fcnt;
	e.t = m_comp->frmtCount;
	e.line = m_comp->vid->ray.y;
	e.dot = m_comp->vid->ray.x;
	return e;
}

void Machine::stampBeam(RunResult& r) const {
	r.beam_line = m_comp->vid->ray.y;
	r.beam_dot = m_comp->vid->ray.x;
	r.beam_t = m_comp->frmtCount;
	r.beam_frame = m_comp->vid->fcnt;
}

// An instruction covers its own bytes, all of them, wherever it went next. The
// PC delta is not a length: a taken `jr c,$+3` moves it by 3 and skips a byte
// that never ran, and a repeating ldir moves it backwards.
void Machine::coverPc(int pcBefore, int len) {
	if (!m_coverage.on) return;
	m_coverage.instructions++;
	m_coverage.mark(pcBefore, pcBefore + (len > 0 ? len : 1) - 1);
}

// The core's own disassembler. Its mnemonic hook alone counts only the opcode
// and prefix bytes - the operands are added while the text is formatted
// (cpu.c:cpuDisasm) - so the length has to come from the whole thing.
int Machine::instrLen(int adr) const {
	int len = 0;
	disasm(adr, &len, nullptr);
	return len > 0 ? len : 1;
}

// z80_exec() either accepts an interrupt or runs the opcode at PC, never both,
// and it decides before anything else happens in the step - so the same test
// on the same fields, made just before compExec(), is the answer and not a
// guess. Mirrors cpu/Z80/z80.c:z80_exec/z80_int: any requested source the
// enable mask lets through gets z80_int() called; an NMI there is always
// taken, an INT only with IFF1 set, not straight after EI, and with the line
// sampled at the end of the previous instruction (flgACK).
bool Machine::interruptDue() const {
	const CPU* cpu = m_comp->cpu;
	if (!cpu || cpu->type != CPU_Z80) return false;
	if (!(cpu->intrq & cpu->inten)) return false;
	if (cpu->intrq & Z80_NMI) return true;
	return (cpu->intrq & Z80_INT) && cpu->flgIFF1 && !cpu->flgNOINT && cpu->flgACK;
}

void Machine::accountInterrupt(int pcBefore, long long t, long long ns) {
	if (m_profile.on) {
		m_profile.t += t;
		m_profile.ns += ns;
		m_profile.interrupts++;
		m_profile.interruptT += t;
		m_profile.interruptNs += ns;
		// the invocations that were running are still running - the handler
		// is about to be charged to them as well, and so is getting there
		for (const auto& f : m_profile.stack) {
			m_profile.ranges[f.range].inclusiveT += t;
			m_profile.ranges[f.range].inclusiveNs += ns;
		}
		// the handler is entered by the interrupt, not from the instruction
		// before it: a ret there must not make the entry read as a return
		m_profile.prevPc = -1;
	}
	if (m_audio.on) audio::tick(m_comp, m_audio, ns, m_comp->vid->fcnt, pcBefore);
}

// Did the instruction at `adr` hand control over by returning? Only ever asked
// at the moment control enters a profiled range, so the memory read costs
// nothing on the hot path. RET, RET cc and the ED 45/4D family (retn/reti and
// their undocumented twins, all of which have bits 0xC7 == 0x45).
// A hand-rolled return - "pop hl / jp (hl)" - is indistinguishable from a jump
// and will read as a call; nothing short of tracking the stack would catch it.
bool Machine::isReturnAt(int adr) const {
	if (adr < 0) return false;
	const int op = readByte(adr) & 0xff;
	if (op == 0xc9) return true;			// ret
	if ((op & 0xc7) == 0xc0) return true;		// ret cc
	if (op == 0xed) return (readByte(adr + 1) & 0xc7) == 0x45;
	return false;
}

void Machine::account(int pc, int sp, long long t, long long ns) {
	if (m_profile.on) {
		m_profile.instructions++;
		m_profile.t += t;
		m_profile.ns += ns;
		m_profile.page[(pc >> 8) & 0xff]++;
		bool any = false;
		int fromReturn = -1;		// answered at most once per instruction
		// Invocations that have returned: SP is back above where it stood when
		// they started. Done before anything is charged, so a routine is not
		// billed for the instruction that follows its own ret.
		while (!m_profile.stack.empty() && sp > m_profile.stack.back().sp)
			m_profile.stack.pop_back();

		for (size_t i = 0; i < m_profile.ranges.size(); i++) {
			auto& r = m_profile.ranges[i];
			const bool in = (pc >= r.start && pc < r.end);
			if (in) {
				r.instructions++;
				r.t += t;
				r.ns += ns;
				if (pc == r.start) r.startHits++;
				if (!r.inside) {
					// arriving from outside. A loop inside the range is not
					// an arrival; a callee returning is an arrival but not a
					// call, or every routine that calls another would count
					// twice and its cost per call would come out halved.
					r.entries++;
					if (fromReturn < 0)
						fromReturn = isReturnAt(m_profile.prevPc) ? 1 : 0;
					if (!fromReturn) r.calls++;
					// One frame per range - the outermost invocation, which is
					// the one that owns the inclusive time. A routine that
					// recurses is already on the stack; so is a caller that a
					// callee has just returned into, which is what keeps a loop
					// of 1500 calls from pushing 1500 frames. Arrivals by jump,
					// or from a PC set by hand, get a frame too: they are the
					// main loop, and it costs whatever runs under it.
					bool onStack = false;
					for (const auto& f : m_profile.stack)
						if (f.range == (int)i) { onStack = true; break; }
					if (!onStack) m_profile.stack.push_back(Profile::Frame{(int)i, sp});
				}
				any = true;
			}
			r.inside = in;
		}

		// Inclusive time: this instruction belongs to every invocation still running.
		for (const auto& f : m_profile.stack) {
			m_profile.ranges[f.range].inclusiveT += t;
			m_profile.ranges[f.range].inclusiveNs += ns;
			m_profile.ranges[f.range].inclusiveInstructions++;
		}

		if (!any) {
			m_profile.otherInstructions++;
			m_profile.otherT += t;
			m_profile.otherNs += ns;
		}
		m_profile.prevPc = pc;
	}
	if (m_trace.on) m_trace.add(pc);
	if (m_audio.on) audio::tick(m_comp, m_audio, ns, m_comp->vid->fcnt, pc);
}

// ---------------------------------------------------------------- lifecycle

Machine::~Machine() {
	if (m_comp) {
		// Eject first, or a machine that ever had a tape aborts on the way
		// out. tape_destroy() frees tap->path and leaves the pointer, then
		// calls tapEject(), which frees it again through tape_set_path(NULL)
		// - glibc catches that as a double free and calls abort(), so the
		// server dies with SIGABRT instead of returning 0. Windows' allocator
		// does not check, which is why it only shows up on Linux.
		// tapEject() is idempotent and NULLs the pointer, so doing it here
		// disarms both frees. Upstream bug, fixed on our side of the fence
		// because the Xpeccy tree is never modified: tape.c:24-38 of
		// 0.6.20260804.
		if (m_comp->tape) tapEject(m_comp->tape);
		compDestroy(m_comp);
	}
	if (m_timingCpu) cpuDestroy(m_timingCpu);
}

bool Machine::init(std::string& err, const std::string& configDir) {
	m_env.load(configDir);
	m_envNote = m_env.note;

	// The core asks the wall clock in three places: mem_cold_fill() picks the
	// cells a noisy pattern comes up wrong in, comp_power_phase() picks the
	// power-on beam position, and cmos.c feeds the RTC. A debugging server has
	// to give the same machine for the same input every run, so the clock the
	// guest sees is pinned: cold RAM noise is reproducible, and a program that
	// reads the clock always reads the same time.
	xhost_time_fixed = 1700000000LL;	// 2023-11-14T22:13:20Z, arbitrary but fixed

	m_comp = compCreate();
	if (!m_comp) { err = "compCreate() failed"; return false; }

	m_memoryKb = m_env.memoryKb;
	m_borderFrac = m_env.borderSize;
	if (!setModel(m_env.model, err)) {
		// an unknown model in the profile must not brick the server
		std::string ignored;
		if (!setModel("Pentagon", ignored)) return false;
		// about the configuration, not about the Pentagon: it stays
		const std::string n = " [model '" + m_env.model + "' unknown, using Pentagon]";
		m_envNote += n;
		m_env.note += n;
	}

	// A machine definition that applied did all of this itself; the old-style
	// configuration decides only when there is none - or when the definition
	// names no roms of its own and nothing ended up loaded.
	if (!m_env.findMachineByHw(m_model)) {
		std::string rerr;
		if (!setRomset(m_env.romsetName, rerr))
			m_env.note += " [romset: " + rerr + "]";

		if (!setLayout(m_env.layoutName, err)) setLayout("default", err);
		compSetBaseFrq(m_comp, m_env.cpuFrq / 1e6);
	} else if (m_romset.empty()) {
		std::string rerr;
		if (!setRomset(m_env.romsetName, rerr))
			m_env.note += " [romset: " + rerr + "]";
	}
	applyPalette();
	// start keeping completed frames, so a blended screenshot works without
	// having had to ask for it in advance
	blend::setDepth(blend::defaults().history);
	reset(RES_DEFAULT);
	return true;
}

std::vector<std::string> Machine::models() const {
	std::vector<std::string> res;
	for (tabHwItem* it = tabHwPtr; it->id != HW_NULL; it++)
		if (it->core && it->core->name) res.push_back(it->core->name);
	return res;
}

// The disk controller a machine physically had, for a machine with no
// definition: a Beta Disk (TR-DOS) on the Soviet ZX clones, the +3's own
// uPD765, and nothing on hardware that had no drive. A definition's own
// `disk = ...` is applied whole in applyMachineDef instead.
static int diskInterfaceFor(HardWare* hw) {
	if (!hw) return DIF_NONE;
	if (hw->id == HW_PLUS3) return DIF_P3DOS;
	if (hw->id == HW_PLUS2A) return DIF_NONE;	// the grey +2 had a tape deck, no drive
	return DIF_BDI;
}

// A machine the core will start and then break the session on: hardware with no
// port handlers leaves `out`/`in` NULL, and the Z80 core calls them unchecked.
// (Upstream's non-ZX machines used to make this list longer; the plus core only
// has Z80 boards, but a NULL handler is still a NULL handler.)
static const char* unsupportedReason(Computer* c) {
	if (!c->hw) return NULL;
	if (c->cpu->type != CPU_Z80)
		return "the core only builds a Z80 now";
	if (!c->hw->out || !c->hw->in)
		return "the core has no IO handlers for it and would segfault on the first port access";
	return NULL;
}

// "1,2,4" - the turbo steps a board declares, x1 always first; the list that
// cannot be read leaves the board with none. xcore/machines.cpp:mac_turbo_tab.
static int turboTabFrom(const std::string& src, double* tab) {
	int cnt = 0;
	tab[cnt++] = 1.0;
	for (const std::string& part : xsp::split(src, ',')) {
		double v = atof(trim(part).c_str());
		if (v < 0.1 || v > 8.0) continue;
		if (v <= tab[cnt - 1]) continue;	// x1 is already there; the list rises
		if (cnt >= TURBO_STEP_MAX) break;
		tab[cnt++] = v;
	}
	return cnt;
}

// What RAM holds when the machine powers on. `ram.cold` is hex byte groups,
// space separated, a group carrying "*N" repeats: "ff*8 00*8" is 8 x ff then
// 8 x 00. `ram.noise` counts the cells in a thousand that come up wrong.
// xcore/machines.cpp:mac_cold_ram.
static void coldRamFill(Computer* c, const std::string& pat, int noise) {
	std::vector<unsigned char> bytes;
	for (const std::string& grp : xsp::split(pat, ' ')) {
		size_t star = grp.find('*');
		std::string hex = (star == std::string::npos) ? grp : grp.substr(0, star);
		int rep = (star == std::string::npos) ? 1 : atoi(grp.c_str() + star + 1);
		std::vector<unsigned char> b;
		for (size_t i = 0; i + 1 < hex.size(); i += 2) {
			// the substring must outlive `end`, which points into it
			const std::string byte = hex.substr(i, 2);
			char* end = nullptr;
			long v = strtol(byte.c_str(), &end, 16);
			if (!end || *end || v < 0 || v > 255) { b.clear(); break; }
			b.push_back((unsigned char)v);
		}
		if (b.empty()) continue;
		while (rep-- > 0) bytes.insert(bytes.end(), b.begin(), b.end());
	}
	if (bytes.empty()) return;
	mem_cold_fill(c->mem, bytes.data(), (int)bytes.size(), noise);
}

// xcore/machines.cpp:mac_set_psg - chip count drives which of the three slots
// are live, and more than one is a TurboSound board, more than two a ZX Next.
static void setPsg(Computer* c, int count, int type, double frq, int stereo) {
	aymChip* psg[3] = {c->ts->chipA, c->ts->chipB, c->ts->chipC};
	for (int i = 0; i < 3; i++) {
		if (!psg[i]) continue;
		psg[i]->stereo = stereo;
		chip_set_type(psg[i], (i < count) ? type : SND_NONE);
	}
	// upstream halves the clock when the chip multiplies it past 4.2MHz
	int mul = chip_frq_mul(type);
	double f = (mul > 1 && frq * mul > 4.2) ? frq / mul : frq;
	ts_set_frq(c->ts, f, c->cpuFrq);	// 0: auto
	c->ts->type = (count > 2) ? TS_ZXNEXT : (count > 1) ? TS_NEDOPC : TS_NONE;
}

// The headless counterpart of xcore/machines.cpp:mac_from_def(). The
// definition is complete - resolve() built it on upstream's defaults - so
// every field is applied: a key the file does not carry must fall to the
// default, not to whatever the previous machine had set.
void Machine::applyMachineDef(const MachineDef& d) {
	Computer* c = m_comp;
	// cpu first, like mac_set_cpu: "z80" picks a plain core, "name@lib" would
	// want the plugin directory we do not carry - skip those
	if (d.cpu.find('@') == std::string::npos)
		cpu_set_type(c->cpu, d.cpu.c_str(), NULL, NULL);
	compSetBaseFrq(c, d.cpuMhz);
	c->turboCount = turboTabFrom(d.turboSteps, c->turboTab);
	c->turboStep = 1;		// a machine comes up on its base clock
	c->hwMul = 1;
	{
		// xcore/machines.cpp:mac_ram_size() semantics, not setMemory()'s
		// api-validation: kb=0 is "the default" (64), and a size the board
		// cannot take is normalized down to the largest it can - refusing
		// here would leave the previous machine's RAM size in place and
		// report that as this machine's
		const int mask = c->hw ? c->hw->mask : 0;
		const int kb = d.memoryKb ? d.memoryKb : 64;
		int size;
		if (kb == 768 && (mask & MEM_768K)) {
			size = MEM_512K + MEM_256K;
		} else {
			long long want = (long long)kb * 1024;
			if (want > MEM_4M) want = MEM_4M;
			if (want < 0) want = 0;
			size = want <= MEM_256 ? MEM_256 : toPowerOfTwo((int)want);
			if (size > MEM_4M) size = MEM_4M;
			if (mask && (~mask & size)) {	// the core has no such size
				size = MEM_4M;
				while (size && !(mask & size)) size >>= 1;
				if (!size) size = MEM_256;
			}
		}
		memSetSize(m_comp->mem, size, -1);
		m_memoryKb = size / 1024;
	}
	// cold fill runs once the size is set: a power-on pattern, not a reset -
	// reset keeps what the ram already holds
	if (!d.ramCold.empty())
		coldRamFill(c, d.ramCold, d.ramNoise);
	c->resbank = d.resbank;
	c->earback = d.earback;
	c->earRC = d.earRC;
	c->fbus = d.fbus;
	c->flgCNTI = d.contio;
	comp_set_cont(c, d.contmem);
	c->flgEM1 = d.scrpwait;
	c->flgDDP = d.ddpal;
	if (c->vid && c->vid->ula) {
		c->vid->ula->conttype = d.contPattern;
		c->vid->ula->early = d.early;
		c->vid->ula->enabled = d.ulaplus;
	}
	if (c->vid) {
		c->vid->brdstep = d.brd4t ? 7 : 1;
		c->vid->brdmin = d.brdmin;
	}
	comp_set_snow(c, d.snow);
	c->flgSNOWX = d.snowx;
	setPsg(c, d.psgCount, d.psgType, d.psgFrq, d.psgStereo);
	c->gs->enable = d.gs;
	c->saa->enabled = d.saa;
	c->sdrv->type = d.soundrive;
	difSetHW(c->dif, d.disk);
	difSetDrives(c->dif, d.drives);
	ide_set_type(c->ide, d.ide);
	c->mouse->enable = d.mouse;
	c->mouse->hasWheel = d.mouseWheel;
	// MAC_JOY_*: 0 none, 1 kempston, 2 kempston with the d5..d7 buttons
	c->joy->type = d.joy ? XJ_KEMPSTON : XJ_NONE;
	c->joy->extbuttons = (d.joy == 2);
	c->keyb->pcmode = d.scantab;
	bool romOk = false;
	if (!d.roms.empty()) {
		Romset rs;
		rs.name = "machine:" + d.id;
		rs.roms = d.roms;
		std::string rerr;
		romOk = loadRomset(rs, rerr);
		if (romOk) m_romset = rs.name;
		else m_env.note += " [romset: " + rerr + "]";
	}
	if (!romOk) {
		// names no roms, or none of them made it in: the machine still has
		// to boot on something - the configured set, then the named 'ZX48',
		// then the rescue image itself. The last step goes by value, not by
		// name: a user romset called "ZX48" is a legitimate thing to shadow
		// with, not a thing to lose the machine over when its files are dead.
		std::string ferr;
		if (setRomset(m_env.romsetName, ferr))
			m_env.note += " [roms: fell back to '" + m_env.romsetName + "']";
		else if (m_env.romsetName != "ZX48" && setRomset("ZX48", ferr))
			m_env.note += " [roms: fell back to 'ZX48']";
		else if (!m_env.rescueRom.roms.empty() && loadRomset(m_env.rescueRom, ferr)) {
			m_romset = m_env.rescueRom.name;
			m_env.note += " [roms: fell back to the rescue rom]";
		} else if (!m_env.lastRom.roms.empty() && loadRomset(m_env.lastRom, ferr)) {
			// the file-based rescue can name a dead user file too; the
			// compiled-in image is the floor nothing else stands on
			m_romset = m_env.lastRom.name;
			m_env.note += " [roms: fell back to the built-in rom]";
		} else if (!ferr.empty())
			m_env.note += " [fallback romset: " + ferr + "]";
	}
	// the GS board's own firmware rom, from the same rom directory; a machine
	// that names one but has the chip off still gets the image - harmless.
	// Upstream clears the chip's rom when the definition names none (or it
	// will not open): the old machine's firmware must not linger into one
	// that never shipped it.
	if (!d.gsRom.empty()) {
		std::string p = romPath(d.gsRom);
		if (platform::isFile(p)) {
			FILE* f = fopen(p.c_str(), "rb");
			if (f) {
				size_t n = fread(c->gs->mem->romData, 1, MEM_32K, f);
				fclose(f);
				c->gs->mem->romSize = (int)n;	// wraps the banking mask
			}
		} else if (const builtin::File* b = builtin::kRoms.find(d.gsRom)) {
			// the shipped firmware compiled into this binary, below a file
			// of the same name in the rom directory as every other rom
			const size_t n = b->size < (size_t)MEM_32K ? b->size : (size_t)MEM_32K;
			memset(c->gs->mem->romData, 0xff, MEM_32K);
			memcpy(c->gs->mem->romData, b->data, n);
			c->gs->mem->romSize = (int)n;
		} else {
			memset(c->gs->mem->romData, 0xff, MEM_32K);
			c->gs->mem->romSize = 0;
			m_env.note += " [gs rom '" + d.gsRom + "' not found]";
		}
	} else {
		memset(c->gs->mem->romData, 0xff, MEM_32K);
		c->gs->mem->romSize = 0;
	}
	// text font the tsconf text mode and the debugger draw from: an empty
	// name drops the previous machine's font (upstream's vid_fnt_del), a
	// missing file is a config error worth a note rather than a silent keep
	if (c->vid) {
		if (d.font.empty()) {
			vid_fnt_del(c->vid);
		} else {
			std::string p = romPath(d.font);
			const builtin::File* b = nullptr;
			if (platform::isFile(p)) {
				vid_fnt_load(c->vid, p.c_str());
			} else if ((b = builtin::kRoms.find(d.font)) && b->size > 0) {
				// what vid_fnt_load() does with a file, from the copy in
				// this binary; realloc because the core frees it with free()
				void* mem = realloc(c->vid->font.data, b->size);
				if (mem) {
					memcpy(mem, b->data, b->size);
					c->vid->font.data = (unsigned char*)mem;
					c->vid->font.size = (int)b->size;
				}
			} else {
				// a dead font name must not leave the last machine's font
				// standing: drop it the way an empty name does
				vid_fnt_del(c->vid);
				m_env.note += " [font '" + d.font + "' not found]";
			}
		}
	}
	// upstream: conf.layName = mac->geometry, then LAY_DEFAULT when the name
	// is missing or unknown - an empty geometry is not "keep the old layout"
	std::string lerr;
	if (!setLayout(d.geometry, lerr) && !setLayout("default", lerr))
		m_env.note += " [geometry '" + d.geometry + "' unknown]";
}

// Names the pinned upstream used where the plus core renamed the machine, so
// a caller asking for what it always asked for still gets one.
static const struct { const char* alias; const char* name; } kModelAliases[] = {
	{"ZX48K", "ZX48"},
	{"Spectrum +2", "Plus2A"},	{"Spectrum +2A", "Plus2A"},
	{"Spectrum +3", "Plus3"},
	{"PentEvo", "Baseconf"},	{"TSLab", "TSConf"},
	{nullptr, nullptr}
};

bool Machine::setModel(const std::string& name, std::string& err) {
	const std::string previous = m_model;
	std::string want = name;
	for (int i = 0; kModelAliases[i].alias; i++)
		if (name == kModelAliases[i].alias) { want = kModelAliases[i].name; break; }
	// a definition stem works as well as the hardware name: the file is
	// "evo-tsconf.conf" while the core calls the hardware "TSConf". The def it
	// resolves to is the one that gets applied - several stems can share one
	// hardware ("zx128" and "zxplus2" are both ZX128 boards with different
	// roms and timing), so looking the def up again by hardware name would
	// lose which of them was asked for.
	const MachineDef* def = nullptr;
	if (want == name) {
		def = m_env.findMachine(name);
		if (!def) {
			std::string low = name;
			for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
			if (low != name) def = m_env.findMachine(low);
		}
		if (def) want = def->hw;
	}
	if (!compSetHardware(m_comp, want.c_str())) {
		err = "unknown machine '" + name + "'";
		return false;
	}
	if (const char* why = unsupportedReason(m_comp)) {
		compSetHardware(m_comp, previous.empty() ? "Pentagon" : previous.c_str());
		err = "'" + name + "' is emulated by the core but not usable here: " + why;
		return false;
	}
	m_model = m_comp->hw->name;
	// what the last machine could not load is not this one's problem
	m_env.note = m_envNote;
	// asked by hardware name, not by stem: the first definition that shares
	// it, which is the same machine the GUI's sorted list would pick
	if (!def) def = m_env.findMachineByHw(m_model);
	m_machineId = def ? def->id : "";
	if (def) {
		// the definition decides everything: contention, snow, rom banks,
		// geometry, memory, cpu clock, the disk interface it shipped with
		applyMachineDef(*def);
	} else {
		// compCreate() leaves the machine with no disk controller at all,
		// because upstream expects the GUI to set one from the machine
		// definition. No definition: give the machine the interface its real
		// hardware came with.
		difSetHW(m_comp->dif, diskInterfaceFor(m_comp->hw));
		// clamp the RAM size to what this hardware supports (xcore/profiles.cpp:535-541)
		std::string ignored;
		setMemory(m_memoryKb, ignored);
	}
	// the picture the previous machine drew does not belong to this one: its
	// lines would be re-read at the new geometry's pitch and shown as one
	// garbage frame before the first real frame lands
	vid_clear_image();
	video::historyClear();
	// applyMachineDef ran compSetBaseFrq -> zx_init -> fdc_set_hd(0); the
	// floppy contents and mounts survive a model switch, their density does
	// not - put it back
	reapplyDriveGeom();
	return true;
}

bool Machine::setMemory(int kb, std::string& err) {
	// Bounded before the multiply, not after: kb * 1024 overflows for anything
	// past two million, and the wrapped value used to land inside the valid
	// range and be accepted as a size the caller never asked for.
	if (kb <= 0) { err = "memory size must be positive"; return false; }
	if (kb > MEM_4M / 1024) {
		err = "memory size must be at most " + std::to_string(MEM_4M / 1024) + "K";
		return false;
	}
	int mask = m_comp->hw ? m_comp->hw->mask : 0;
	// xcore/machines.cpp:mac_ram_size - 768K is not a power of two, it is
	// 512+256 fitted behind the 1M mask, and only where the board allows it
	int size;
	if (kb == 768 && (mask & MEM_768K)) {
		size = MEM_512K + MEM_256K;
	} else {
		size = toPowerOfTwo(kb * 1024);
		if (size < MEM_256) size = MEM_256;
		if (size > MEM_4M) size = MEM_4M;
		if (mask && (~mask & size)) {		// not supported: take the largest that is
			size = MEM_4M;
			while (size && !(mask & size)) size >>= 1;
			if (!size) { err = "hardware supports no known RAM size"; return false; }
		}
	}
	memSetSize(m_comp->mem, size, -1);
	m_memoryKb = size / 1024;
	return true;
}

// Where a machine's rom file actually is: the user's rom directory first, so
// a file there shadows one that ships; then the roms that ship with the core.
// Upstream gets the same effect by copying the shipped roms into the user's
// directory the first time it runs (config.cpp seedRom) - a headless server
// leaves the user's directory alone and searches both instead. An absolute
// name is used as given (xm_rom_path).
std::string Machine::romPath(const std::string& name) const {
	if (name.empty()) return name;
	if (name.front() == '/' || (name.size() > 2 && name[1] == ':' &&
	    (name[2] == '/' || name[2] == '\\')))
		return name;
	std::string p;
	if (!m_env.romDir.empty()) {
		p = platform::join(m_env.romDir, name);
		if (platform::isFile(p)) return p;
	}
	if (!m_env.romDirShip.empty()) {
		std::string q = platform::join(m_env.romDirShip, name);
		if (platform::isFile(q)) return q;
	}
	return p.empty() ? name : p;	// errors name the primary directory
}

// mirrors xcore/machines.cpp:mac_load_rom(), with one difference: the image
// is built aside and only replaces the machine's ROM once something loaded.
// Upstream fills the ROM with FF first, so a romset whose files are all gone
// leaves a machine that reads FF everywhere while the name says otherwise.
bool Machine::loadRomset(const Romset& rs, std::string& err) {
	std::vector<unsigned char> image(MEM_512K, 0xff);
	int romsz = MEM_256;
	int loaded = 0;
	for (const auto& rf : rs.roms) {
		// the bytes come from a file in the romset directory, or - for the ROM
		// compiled into this binary - from the image we are already holding
		std::vector<unsigned char> src;
		if (rf.data) {
			src.assign(rf.data, rf.data + rf.dataSize);
		} else {
			std::string path = romPath(rf.name);
			FILE* f = fopen(path.c_str(), "rb");
			if (!f) {
				if (!err.empty()) err += "; ";
				err += "can't open '" + path + "'";
				continue;
			}
			fseek(f, 0, SEEK_END);
			long len = ftell(f);
			rewind(f);
			if (len > 0) {
				src.resize((size_t)len);
				len = (long)fread(src.data(), 1, src.size(), f);
				src.resize(len > 0 ? (size_t)len : 0);
			}
			fclose(f);
		}
		if (src.empty()) {
			if (!err.empty()) err += "; ";
			err += "'" + rf.name + "' is empty";
			continue;
		}

		// 64-bit until every bound is proven: a definition can write
		// rom1 = x.rom:0:2097140, and 2097140K * 1024 sits beside INT_MAX
		// where int overflow makes the size checks below see garbage
		long long foff = (long long)rf.foffset * 1024;
		long long roff = (long long)rf.roffset * 1024;
		long long fsze = rf.fsize > 0 ? (long long)rf.fsize * 1024 : (long long)src.size();
		if (roff + fsze > romsz) {
			long long want = roff + fsze;
			if (want > MEM_512K) want = MEM_512K;
			romsz = toPowerOfTwo((int)want);
			if (romsz < MEM_256) romsz = MEM_256;
			if (romsz > MEM_512K) romsz = MEM_512K;
		}
		if (roff + fsze > romsz) fsze = romsz - roff;
		if (foff < 0 || foff >= (long long)src.size()) continue;
		if (foff + fsze > (long long)src.size()) fsze = (long long)src.size() - foff;
		// the destination side of the copy too: fsze rebounded from the
		// source file can exceed what romsz leaves above roff
		if (roff >= 0 && roff < MEM_512K && fsze > 0) {
			if (roff + fsze > MEM_512K) fsze = MEM_512K - roff;
			memcpy(image.data() + roff, src.data() + foff, fsze);
			loaded++;
		}
	}
	if (!loaded) {
		if (err.empty()) err = "romset '" + rs.name + "' has no usable files";
		return false;
	}
	// A short rom is padded, not mirrored: an 8K file leaves FF above $2000,
	// as upstream does, rather than a mask that repeats it there.
	if (romsz < MEM_16K) romsz = MEM_16K;
	memcpy(m_comp->mem->romData, image.data(), MEM_512K);
	memSetSize(m_comp->mem, -1, romsz);
	err.clear();
	return true;
}

bool Machine::setRomset(const std::string& name, std::string& err) {
	const Romset* rs = m_env.findRomset(name);
	if (!rs) { err = "unknown romset '" + name + "'"; return false; }
	if (!loadRomset(*rs, err)) return false;
	m_romset = name;
	return true;
}

// The core keeps border size as a VID_BRD_* mode; this server speaks in a
// fraction of the full 48-dot border. The steps are uneven (TINY is 8 dots,
// MEDIUM 32), so the fraction picks the nearest mode by its x margin, and
// borderSize() reports back what was asked for rather than the quantized mode.
static const int kBrdMarginX[] = {0, 8, 16, 32, 48, 48};	// VID_BRD_* -> dots

static int borderModeFor(double frac) {
	int dots = (int)(frac * 48 + 0.5);
	int mode = VID_BRD_NONE;
	for (int i = 0; i <= VID_BRD_FULL; i++) {
		int lo = i ? (kBrdMarginX[i - 1] + kBrdMarginX[i]) / 2 : -1;
		if (dots >= lo) mode = i;
	}
	return mode;
}

void Machine::applyLayout(const Layout& lay) {
	vLayout vl;
	vl.full.x = lay.full_x;   vl.full.y = lay.full_y;
	vl.bord.x = lay.bord_x;   vl.bord.y = lay.bord_y;
	vl.blank.x = lay.blank_x; vl.blank.y = lay.blank_y;
	vl.scr.x = lay.scr_x;     vl.scr.y = lay.scr_y;
	vl.intpos.x = lay.int_x;  vl.intpos.y = lay.int_y;
	vl.intSize = lay.int_size;
	m_comp->vid->brdmode = borderModeFor(m_borderFrac);
	comp_set_layout(m_comp, &vl);		// uses hw->lay when the hardware fixes one
	vid_upd_layout(m_comp->vid);		// re-apply with our border mode
	video::applyGeometry(m_comp);		// globals xgui would set (bytesPerLine etc.)
}

double Machine::borderSize() const { return m_borderFrac; }

void Machine::setBorderSize(double frac) {
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	m_borderFrac = frac;
	m_comp->vid->brdmode = borderModeFor(frac);
	vid_upd_crop(m_comp->vid);		// only the crop moves, not the timings
	video::applyGeometry(m_comp);		// vsze moved, so the buffer geometry did
}

bool Machine::setLayout(const std::string& name, std::string& err) {
	// Xpeccy-plus calls its shipped layouts "ULA.Pentagon" where the upstream
	// config called the same thing "Pentagon" - a caller that only knows the
	// old spelling still gets there
	const Layout* lay = m_env.findLayout(name);
	if (!lay && name.find("ULA.") != 0) lay = m_env.findLayout("ULA." + name);
	if (!lay) { err = "unknown geometry '" + name + "'"; return false; }
	applyLayout(*lay);
	m_layout = name;
	return true;
}

// default ZX palette (xcore/palette.cpp:80-88); without it every frame is black
void Machine::applyPalette() {
	for (int i = 0; i < 16; i++) {
		xColor col;
		col.b = (i & 1) ? ((i & 8) ? 0xff : 0xaa) : 0x00;
		col.r = (i & 2) ? ((i & 8) ? 0xff : 0xaa) : 0x00;
		col.g = (i & 4) ? ((i & 8) ? 0xff : 0xaa) : 0x00;
		vid_set_bcol(m_comp->vid, i, col);
		vid_set_col(m_comp->vid, i, col);
	}
}

// zx_init re-marks every drive DD (trklen 6250), and it is reached from every
// reset, timing change and even a port write that flips turbo - a mounted
// HD image keeps its insert flag but stops reading. noteDriveGeom remembers
// what each mount was formatted with; reapplyDriveGeom puts it back.
void Machine::noteDriveGeom(int drv, long trklen) {
	if (drv >= 0 && drv < 4) m_driveGeom[drv] = trklen;
}

void Machine::reapplyDriveGeom() {
	if (!m_comp || !m_comp->dif) return;
	if (!m_driveGeom[0] && !m_driveGeom[1] && !m_driveGeom[2] && !m_driveGeom[3])
		return;
	for (int i = 0; i < 4; i++) {
		Floppy* f = m_comp->dif->flp[i];
		if (f && f->insert && m_driveGeom[i] > 0 && f->trklen != m_driveGeom[i])
			f->trklen = (int)m_driveGeom[i];
	}
}

long long Machine::execOne() {
	const long long ns = compExec(m_comp);
	reapplyDriveGeom();
	return ns;
}

void Machine::reset(int mode) {
	compReset(m_comp, mode);
	reapplyDriveGeom();
	// the frames before a reset belong to another program; blending across it
	// would average two unrelated pictures
	video::historyClear();
	// compCreate/compReset leave the key matrix all-zero, which reads as "every
	// key held down" - the GUI clears it via comp_kbd_release() and so must we.
	kbdReleaseAll(m_comp->keyb);
	m_comp->flgBRK = 0;
}

// ---------------------------------------------------------------- execution

RunResult Machine::execLoop(long long maxInstructions, int stopPc, int maxFrames, int stopSp,
			    int beamLine, int beamDot) {
	RunResult r;
	Computer* c = m_comp;
	c->flgBRK = 0;
	const int savedDebug = c->flgDBG;
	bool first = true;

	// A raster target behind the beam means the next frame, not "stop at once",
	// so the frame it is aimed at is settled before the loop starts rather than
	// re-decided on every instruction.
	bool beamArmed = false;
	if (beamLine >= 0) {
		const int nowLine = c->vid->ray.y, nowDot = c->vid->ray.x;
		const bool reached = nowLine > beamLine ||
				     (nowLine == beamLine && (beamDot < 0 || nowDot >= beamDot));
		beamArmed = !reached;	// already past it: wait for the frame to wrap first
	}

	while (true) {
		if (maxInstructions >= 0 && r.instructions >= maxInstructions) {
			r.reason = "budget";
			break;
		}
		// standing on a breakpoint: run one opcode with checks off, or we'd
		// break on the spot forever (same trick as xThread::emuCycle)
		c->flgDBG = first ? 1 : savedDebug;
		const int pcBefore = cpu_get_pc(c->cpu);
		const int spBefore = cpu_get_sp(c->cpu);
		const bool watched = m_beamLog.on && m_beamLog.watching(pcBefore);
		const BeamLog::Event ev = watched ? beamEvent(pcBefore) : BeamLog::Event();
		const bool intDue = interruptDue();
		const int len = (m_coverage.on && !intDue) ? instrLen(pcBefore) : 1;
		// tickCount is the exact integer T the cpu charges, waits included -
		// kept by the core as a signed int that wraps past INT_MAX. The
		// per-instruction delta stays right through the wrap as an unsigned
		// difference (one instruction never comes near 2^31 T).
		const int t0 = c->tickCount;
		const long long ns = execOne();
		const int t = (int)((unsigned)c->tickCount - (unsigned)t0);
		c->flgDBG = savedDebug;
		first = false;
		r.t += t;
		r.ns += ns;
		// flgBRKPRE: compExec returned before the opcode ran - a fetch break
		// or the irq break - so nothing executed to count, profile or cover.
		// An accepted interrupt ran no opcode either: the instruction at
		// pcBefore runs after the handler returns, and is counted then.
		if (!c->flgBRKPRE) {
			if (intDue && !c->cpu->flgIFF1) {
				r.interrupts++;
				accountInterrupt(pcBefore, t, ns);
			} else {
				r.instructions++;
				account(pcBefore, spBefore, t, ns);
				coverPc(pcBefore, len);
				if (watched) m_beamLog.add(ev);
			}
		}

		if (c->flgFRM) {
			c->flgFRM = 0;
			r.frames++;
			if (m_profile.on) m_profile.frames++;
			video::historyPush(c);		// for blended captures
			// the frame wrapped, so a target that was behind us is ahead again
			if (beamLine >= 0) beamArmed = true;
		}
		if (c->flgBRK) {
			r.reason = "breakpoint";
			r.brk_type = c->brkt;
			r.brk_addr = c->brka;
			c->flgBRK = 0;
			break;
		}
		if (beamArmed) {
			const int line = c->vid->ray.y, dot = c->vid->ray.x;
			if (line > beamLine || (line == beamLine && (beamDot < 0 || dot >= beamDot))) {
				r.reason = "beam";
				break;
			}
		}
		if (maxFrames >= 0 && r.frames >= maxFrames) {
			r.reason = "frames";
			break;
		}
		int pc = cpu_get_pc(c->cpu);
		if (stopPc >= 0 && pc == stopPc) {
			r.reason = "pc";
			break;
		}
		if (stopSp >= 0 && cpu_get_sp(c->cpu) > stopSp) {
			r.reason = "return";
			break;
		}
	}
	r.pc = cpu_get_pc(c->cpu);
	stampBeam(r);
	return r;
}

RunResult Machine::run(long long maxInstructions, int stopPc, int maxFrames) {
	return execLoop(maxInstructions, stopPc, maxFrames, -1);
}

RunResult Machine::runToBeam(int line, int dot, long long maxInstructions) {
	RunResult r = execLoop(maxInstructions, -1, -1, -1, line, dot);
	if (r.reason.empty()) r.reason = "beam";
	return r;
}

RunResult Machine::runFrames(int count) {
	RunResult r = execLoop(-1, -1, count, -1);
	return r;
}

RunResult Machine::step(int count) {
	if (count < 1) count = 1;
	RunResult r;
	Computer* c = m_comp;
	const int savedDebug = c->flgDBG;
	c->flgDBG = 1;				// stepping ignores breakpoints
	// A step is one compExec: accepting an interrupt is a step of its own that
	// lands on the handler, the way a debugger shows it, but it is reported
	// as an interrupt and not as an instruction at the PC it interrupted.
	for (int i = 0; i < count; i++) {
		const int pcBefore = cpu_get_pc(c->cpu);
		const int spBefore = cpu_get_sp(c->cpu);
		const bool watched = m_beamLog.on && m_beamLog.watching(pcBefore);
		const BeamLog::Event ev = watched ? beamEvent(pcBefore) : BeamLog::Event();
		const bool intDue = interruptDue();
		const int len = (m_coverage.on && !intDue) ? instrLen(pcBefore) : 1;
		const int t0 = c->tickCount;
		const long long ns = execOne();
		const int t = (int)((unsigned)c->tickCount - (unsigned)t0);
		r.t += t;
		r.ns += ns;
		if (intDue && !c->cpu->flgIFF1) {
			r.interrupts++;
			accountInterrupt(pcBefore, t, ns);
		} else {
			r.instructions++;
			account(pcBefore, spBefore, t, ns);
			coverPc(pcBefore, len);
			if (watched) m_beamLog.add(ev);
		}
		if (c->flgFRM) {
			c->flgFRM = 0;
			r.frames++;
			if (m_profile.on) m_profile.frames++;
			video::historyPush(c);
		}
	}
	c->flgDBG = savedDebug;
	c->flgBRK = 0;
	r.pc = cpu_get_pc(c->cpu);
	r.reason = "steps";
	stampBeam(r);
	return r;
}

// Step over CALL/RST/block ops: put a one-shot stop right after the opcode.
RunResult Machine::stepOver(long long maxInstructions) {
	int len = 0;
	xMnem mn;
	disasm(cpu_get_pc(m_comp->cpu), &len, &mn);
	if (!(mn.flag & OF_SKIPABLE) || len <= 0)
		return step(1);
	int back = (cpu_get_pc(m_comp->cpu) + len) & 0xffff;
	RunResult r = execLoop(maxInstructions, back, -1, -1);
	if (r.reason.empty()) r.reason = "step_over";
	return r;
}

RunResult Machine::stepOut(long long maxInstructions) {
	int sp = cpu_get_sp(m_comp->cpu);
	RunResult r = execLoop(maxInstructions, -1, -1, sp);
	if (r.reason.empty()) r.reason = "step_out";
	return r;
}

// ---------------------------------------------------------------- frame cost
//
// An effect frame is usually longer than a hardware frame: the code renders,
// waits for the interrupt with HALT, and if the render overran, the interrupt
// it waits for is the second or third one. run_frames counts interrupts, which
// is the wrong unit for "does my effect fit". This counts the work between two
// frame boundaries and keeps the HALT wait separate, so work + idle is the wall
// clock and idle is exactly the headroom.
FrameCost Machine::frameCost(int sync, long long maxInstructions, bool instrument) {
	FrameCost fc;
	Computer* c = m_comp;
	const int savedDebug = c->flgDBG;
	c->flgDBG = 1;			// breakpoints must not interrupt a measurement
	c->flgFRM = 0;

	while (fc.instructions < maxInstructions) {
		const int haltedBefore = c->cpu->flgHALT;
		const int pcBefore = cpu_get_pc(c->cpu);
		const int spBefore = cpu_get_sp(c->cpu);
		const bool intDue = interruptDue();
		const int len = (instrument && m_coverage.on && !intDue) ? instrLen(pcBefore) : 1;
		const int t0 = c->tickCount;
		const long long ns = execOne();
		const int t = (int)((unsigned)c->tickCount - (unsigned)t0);
		const int haltedAfter = c->cpu->flgHALT;
		const bool intTaken = intDue && !c->cpu->flgIFF1;
		if (!intTaken) fc.instructions++;
		// the profiler, trace and coverage see this exactly as they see a run,
		// so one pass can measure the frame and profile it at the same time
		if (instrument) {
			if (intTaken) {
				accountInterrupt(pcBefore, t, ns);
			} else {
				account(pcBefore, spBefore, t, ns);
				coverPc(pcBefore, len);
			}
		}
		// parked in HALT and still parked: pure waiting. Leaving HALT is the
		// interrupt being taken, which is real work and belongs to this frame.
		if (haltedBefore && haltedAfter) fc.idle_t += t;
		else fc.work_t += t;
		if (c->flgFRM) {
			c->flgFRM = 0;
			fc.interrupts++;
			if (instrument && m_profile.on) m_profile.frames++;
			video::historyPush(c);
		}

		if (sync < 0) {
			// entering HALT ends the frame; its own 4 T count as work
			if (!haltedBefore && haltedAfter) { fc.complete = true; break; }
		} else if (cpu_get_pc(c->cpu) == sync) {
			fc.complete = true;
			break;
		}
	}
	c->flgDBG = savedDebug;
	return fc;
}

// ---------------------------------------------------------------- instruction timing
//
// Hand-written T-state tables are exactly the thing this server exists to
// replace, so the numbers come from the emulator: the instruction is executed
// in a sandbox CPU with its own 64K of memory and dead I/O, and we read the
// T-states it charged. Conditional instructions are run twice, once with every
// flag set and once with none, which covers both outcomes of every condition
// the Z80 has; B is set so djnz branches the same way.
namespace {

struct TimingSandbox {
	unsigned char mem[0x10000] = {0};
};

int sandboxRd(int adr, int m1, void* ptr) {
	(void)m1;
	return ((TimingSandbox*)ptr)->mem[adr & 0xffff];
}
void sandboxWr(int adr, int val, void* ptr) {
	((TimingSandbox*)ptr)->mem[adr & 0xffff] = (unsigned char)val;
}
int sandboxIn(int adr, void* ptr) { (void)adr; (void)ptr; return 0xff; }
void sandboxOut(int adr, int val, void* ptr) { (void)adr; (void)val; (void)ptr; }
int sandboxAck(void* ptr) { (void)ptr; return 0xff; }
void sandboxIrq(int t, void* ptr) { (void)t; (void)ptr; }

TimingSandbox g_sandbox;

} // namespace

InstrTiming Machine::instrTiming(int adr) {
	InstrTiming t;
	if (!m_timingCpu) {
		m_timingCpu = cpuCreate(CPU_Z80, sandboxRd, sandboxWr, sandboxIn, sandboxOut,
					sandboxAck, sandboxIrq, &g_sandbox);
		if (!m_timingCpu) return t;
	}
	// the instruction and enough tail for a 4-byte opcode, at the address it
	// really lives at, so relative jumps land where they would land
	adr &= 0xffff;
	for (int i = 0; i < 8; i++)
		g_sandbox.mem[(adr + i) & 0xffff] = (unsigned char)readByte(adr + i);

	int res[2] = {0, 0};
	for (int pass = 0; pass < 2; pass++) {
		cpu_reset(m_timingCpu);
		cpu_set_pc(m_timingCpu, adr);
		cpu_set_reg(m_timingCpu, "SP", 0xff00);
		cpu_set_flag(m_timingCpu, pass ? 0x00 : 0xff);
		cpu_set_reg(m_timingCpu, "BC", pass ? 0x0100 : 0x0200);	// djnz: B=1 falls through
		m_timingCpu->flgHALT = 0;
		m_timingCpu->intrq = 0;
		res[pass] = cpu_exec(m_timingCpu);
	}
	t.taken = res[0] > res[1] ? res[0] : res[1];
	t.not_taken = res[0] > res[1] ? res[1] : res[0];
	t.conditional = (res[0] != res[1]);
	return t;
}

// Step until the source line changes - the listing tells us which addresses
// belong to the line we started on.
RunResult Machine::stepLine() {
	RunResult r;
	if (!m_listing.size()) {
		r = step(1);
		r.reason = "steps (no listing loaded)";
		return r;
	}
	const SourceLine* start = m_listing.coveringAddress(cpu_get_pc(m_comp->cpu));
	const long long budget = 2000000;
	while (r.instructions < budget) {
		RunResult one = step(1);
		r.instructions += one.instructions;
		r.interrupts += one.interrupts;
		r.t += one.t;
		r.ns += one.ns;
		r.frames += one.frames;
		const SourceLine* now = m_listing.coveringAddress(cpu_get_pc(m_comp->cpu));
		if (!now) continue;			// inside a call into ROM, keep going
		if (!start || now->line != start->line || now->file != start->file) {
			r.reason = "line";
			break;
		}
	}
	if (r.reason.empty()) r.reason = "budget";
	r.pc = cpu_get_pc(m_comp->cpu);
	return r;
}

// ---------------------------------------------------------------- memory

int Machine::readByte(int adr) const {
	return memRd(m_comp->mem, adr & 0xffff) & 0xff;
}

void Machine::writeByte(int adr, int val) {
	memWr(m_comp->mem, adr & 0xffff, val & 0xff);
}

int Machine::bankCount() const {
	return m_comp->mem->ramSize / 0x4000;
}

int Machine::readBankByte(int bank, int off) const {
	if (bank < 0 || bank >= bankCount()) return -1;
	if (off < 0 || off > 0x3fff) return -1;	// past the bank is another bank
	return m_comp->mem->ramData[bank * 0x4000 + off];
}

int Machine::findBytes(const std::vector<unsigned char>& pat, int from, int to) const {
	if (pat.empty()) return -1;
	if (from < 0) from = 0;
	if (to < 0 || to > 0xffff) to = 0xffff;
	for (int a = from; a + (int)pat.size() - 1 <= to; a++) {
		size_t i = 0;
		while (i < pat.size() && readByte(a + i) == pat[i]) i++;
		if (i == pat.size()) return a;
	}
	return -1;
}

// ---------------------------------------------------------------- code

static int dasmRd(int adr, void* ptr) {
	Computer* comp = (Computer*)ptr;
	return memRd(comp->mem, adr & 0xffff);
}

std::string Machine::disasm(int adr, int* len, xMnem* mn) const {
	char buf[256] = {0};
	xMnem m = cpuDisasm(m_comp->cpu, adr & 0xffff, buf, dasmRd, m_comp);
	if (len) *len = m.len;
	if (mn) *mn = m;
	return std::string(buf);
}

// xpeccy's built-in assembler wants decimal or 0x; ZX sources are full of
// $/#/% numbers, so normalise them before handing the line over.
static std::string normalizeAsm(const std::string& src) {
	std::string out;
	out.reserve(src.size() + 8);
	for (size_t i = 0; i < src.size(); i++) {
		char c = src[i];
		int base = 0;
		if (c == '$' || c == '#') base = 16;
		else if (c == '%') base = 2;
		if (base) {
			size_t j = i + 1;
			std::string digits;
			while (j < src.size() && isalnum((unsigned char)src[j])) {
				char d = (char)toupper((unsigned char)src[j]);
				bool okDigit = (base == 16)
					? ((d >= '0' && d <= '9') || (d >= 'A' && d <= 'F'))
					: (d == '0' || d == '1');
				if (!okDigit) break;
				digits += d;
				j++;
			}
			if (!digits.empty() && (j >= src.size() || !isalnum((unsigned char)src[j]))) {
				out += std::to_string(strtol(digits.c_str(), nullptr, base));
				i = j - 1;
				continue;
			}
		}
		out += c;
	}
	return out;
}

static bool isAsmIdentChar(char c) {
	return isalnum((unsigned char)c) || c == '_' || c == '.';
}

// The core's assembler wants exactly one space in the line - between the
// mnemonic and its operands - and none anywhere else. "ld a, 5" is refused, and
// so are "ld  a,5" and "ld a,(ix + 5)", which is how most people write Z80 and
// therefore the first thing they hit. Nothing here has string literals, so
// squeezing every other space out is safe.
static std::string squeezeAsm(const std::string& src) {
	std::string s = trim(src);
	size_t head = 0;
	while (head < s.size() && !isspace((unsigned char)s[head])) head++;
	std::string out = s.substr(0, head), operands;
	for (size_t i = head; i < s.size(); i++)
		if (!isspace((unsigned char)s[i])) operands += s[i];
	if (!operands.empty()) out += " " + operands;
	return out;
}

// "loop:" or "loop: ld a,1" - an identifier at the start of the line followed by
// a colon. No Z80 mnemonic looks like that, so there is nothing to disambiguate.
static bool splitLabelDef(std::string& text, std::string& name) {
	size_t i = 0;
	while (i < text.size() && isAsmIdentChar(text[i])) i++;
	if (i == 0 || isdigit((unsigned char)text[0])) return false;
	size_t j = i;
	while (j < text.size() && isspace((unsigned char)text[j])) j++;
	if (j >= text.size() || text[j] != ':') return false;
	name = text.substr(0, i);
	text = trim(text.substr(j + 1));
	return true;
}

// A label with one of these names would be substituted into the place a register
// or a condition belongs, and the result is worse than an error: "c: ... jr c,x"
// assembles cleanly as an unconditional-looking jump to a number. Refused by
// name instead.
static bool isReservedAsmName(const std::string& s) {
	static const std::set<std::string> reserved = {
		"a", "b", "c", "d", "e", "h", "l", "i", "r", "f",
		"af", "bc", "de", "hl", "sp", "pc", "ix", "iy",
		"ixh", "ixl", "iyh", "iyl",
		"z", "nz", "nc", "po", "pe", "p", "m"
	};
	std::string low;
	for (char ch : s) low += (char)tolower((unsigned char)ch);
	return reserved.count(low) > 0;
}

// Replaces the names this block defines. Anything else is left exactly as it is,
// so registers and operands are never touched: a word only becomes a number if
// it is one of ours. The leading word is skipped because that is always the
// mnemonic - which is what lets a label be called `ret` without breaking `ret`.
// Names declared further down stand in as `pending` until the first pass has
// placed them.
static std::string substituteLocal(const std::string& text,
				   const std::map<std::string, int>& known,
				   const std::set<std::string>& declared, int pending) {
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	bool firstWord = true;
	while (i < text.size()) {
		if (!isAsmIdentChar(text[i]) || isdigit((unsigned char)text[i])) {
			out += text[i++];
			continue;
		}
		size_t j = i;
		while (j < text.size() && isAsmIdentChar(text[j])) j++;
		std::string word = text.substr(i, j - i);
		if (firstWord) {
			out += word;			// the mnemonic
			firstWord = false;
		} else {
			auto it = known.find(word);
			if (it != known.end()) out += std::to_string(it->second);
			else if (declared.count(word)) out += std::to_string(pending);
			else out += word;
		}
		i = j;
	}
	return out;
}

std::vector<AsmLine> Machine::assemble(int adr, const std::vector<std::string>& lines, bool write,
				       std::map<std::string, int>* defined) {
	// Strip the source once so both passes see the same instructions, and take
	// the label definitions off the front of their lines.
	struct Src { std::string text, label; };
	std::vector<Src> src;
	std::set<std::string> declared;
	for (const auto& raw : lines) {
		std::string text = trim(raw);
		size_t cmt = text.find(';');
		if (cmt != std::string::npos) text = trim(text.substr(0, cmt));
		if (text.empty()) continue;
		Src s;
		if (splitLabelDef(text, s.label)) {
			if (isReservedAsmName(s.label)) {
				AsmLine bad;
				bad.text = trim(raw);
				bad.address = adr & 0xffff;
				bad.error = "'" + s.label + "' is a register or condition name, "
					    "not usable as a label";
				return { bad };
			}
			if (!declared.insert(s.label).second) {
				AsmLine bad;
				bad.text = trim(raw);
				bad.address = adr & 0xffff;
				bad.error = "label '" + s.label + "' is defined twice";
				return { bad };
			}
		}
		s.text = text;
		if (s.text.empty() && s.label.empty()) continue;
		src.push_back(s);
	}

	// First pass: place the labels. A name we have not reached yet stands in as
	// the address of the instruction using it, which keeps jr and djnz within
	// range and is a valid 16-bit operand; instruction length on a Z80 follows
	// the mnemonic rather than the operand's value, so the addresses this
	// produces are the final ones.
	std::map<std::string, int> local;
	int cur = adr & 0xffff;
	for (const auto& s : src) {
		if (!s.label.empty()) local[s.label] = cur;
		if (s.text.empty()) continue;
		char buf[64] = {0};
		std::string line = squeezeAsm(normalizeAsm(m_labels.substitute(substituteLocal(s.text, local, declared, cur))));
		int len = cpuAsm(m_comp->cpu, line.c_str(), buf, cur);
		if (len <= 0) break;		// the second pass reports it, with the real text
		cur = (cur + len) & 0xffff;
	}

	// Second pass: assemble for real, now that every label has an address.
	std::vector<AsmLine> res;
	cur = adr & 0xffff;
	for (const auto& s : src) {
		if (!s.label.empty()) local[s.label] = cur;
		if (s.text.empty()) continue;

		AsmLine al;
		al.text = s.text;
		al.address = cur;

		// our own labels first, then the loaded symbol table (the core's
		// assembler has no symbol table at all), then the numbers
		char buf[64] = {0};
		std::string line = squeezeAsm(normalizeAsm(m_labels.substitute(substituteLocal(s.text, local, declared, cur))));
		int len = cpuAsm(m_comp->cpu, line.c_str(), buf, cur);
		if (len <= 0) {
			al.ok = false;
			al.error = "can't assemble";
			res.push_back(al);
			break;			// stop at the first bad line, like a real assembler
		}
		for (int i = 0; i < len; i++) al.bytes.push_back((unsigned char)buf[i]);
		al.ok = true;
		if (write) {
			for (int i = 0; i < len; i++)
				writeByte(cur + i, (unsigned char)buf[i]);
		}
		cur = (cur + len) & 0xffff;
		res.push_back(al);
	}
	if (defined) *defined = local;
	return res;
}

// ---------------------------------------------------------------- breakpoints

// The core stores cell breakpoints in physical maps indexed by absolute address
// (spectrum.c:getBrkPtr), which is what "the breakpoint follows the bank"
// actually means. Going through those maps directly is the only way to arm a
// cell that is not paged in at the moment, and the only way to disarm exactly
// the cell we armed.
unsigned char* Machine::brkCell(int type, int idx) const {
	if (idx < 0) return nullptr;
	switch (type) {
		case MEM_RAM:
			if (idx >= (int)sizeof(m_comp->brkRamMap)) return nullptr;
			return m_comp->brkRamMap + idx;
		case MEM_ROM:
			if (idx >= (int)sizeof(m_comp->brkRomMap)) return nullptr;
			return m_comp->brkRomMap + idx;
	}
	return nullptr;
}

// The maps are fields of the Computer and survive a machine switch, a RAM size
// change and a romset change untouched; only the mask that indexes them moves.
// So the cell is the one the mask picks now, and from then on it is that index.
void Machine::armCell(int type, int abs, unsigned char flags) {
	int idx = -1;
	switch (type) {
		case MEM_RAM: idx = abs & m_comp->mem->ramMask; break;
		case MEM_ROM: idx = abs & m_comp->mem->romMask; break;
	}
	unsigned char* cell = brkCell(type, idx);
	if (!cell) return;
	*cell = (unsigned char)((*cell & 0xf0) | (flags & 0x0f));
	// compExec only looks at the maps while this is set; Xpeccy-plus' own setter
	// (setBrk) raises it, the direct write here has to do it itself
	if (flags & 0x0f) m_comp->flgBRKMEM = 1;
	for (const auto& b : m_breakList)
		if (b.type == type && b.idx == idx) return;
	m_breakList.push_back(PhysBreak{type, idx});
}

void Machine::setBreak(int adr, unsigned char flags) {
	xAdr xa = mem_get_xadr(m_comp->mem, adr & 0xffff);
	armCell(xa.type, xa.abs, flags);
}

bool Machine::setBreakBank(int bank, int offset, unsigned char flags, std::string& err) {
	// Refused rather than masked: bank 15 on a 128K machine is bank 7 to the
	// RAM mask, and a breakpoint that fires in a bank nobody named is worse
	// than an error. Same bound readBankByte() keeps.
	if (bank < 0 || bank >= bankCount()) {
		err = "no RAM bank " + std::to_string(bank) + " (the machine has " +
		      std::to_string(bankCount()) + ")";
		return false;
	}
	if (offset < 0 || offset > 0x3fff) {
		err = "offset " + std::to_string(offset) + " is outside a 16K bank ($0000..$3FFF)";
		return false;
	}
	// A symbol file counts in 16K banks. The core's own page number is in units
	// of its page size - 256 bytes, which is why machine_state shows bank 2 as
	// 128 - so go through the absolute address rather than the page number.
	armCell(MEM_RAM, bank * 0x4000 + offset, flags);
	return true;
}

void Machine::setBreakAddress(int adr, unsigned char flags) {
	unsigned char& cell = m_comp->brkAdrMap[adr & 0xffff];
	cell = (unsigned char)((cell & 0xf0) | (flags & 0x0f));
	if (flags & 0x0f) m_comp->flgBRKMEM = 1;
}

int Machine::physAddress(int adr) const {
	return mem_get_xadr(m_comp->mem, adr & 0xffff).abs;
}

// The 16K bank a CPU address currently sits in - the unit a symbol file uses,
// not the core's 256-byte page number.
int Machine::bankAt(int adr) const {
	return physAddress(adr) / 0x4000;
}

unsigned char Machine::getBreak(int adr) const {
	return getBrk(m_comp, adr & 0xffff);
}

void Machine::clearBreaks() {
	for (const auto& b : m_breakList) {
		if (unsigned char* cell = brkCell(b.type, b.idx))
			*cell &= 0xf0;
	}
	m_breakList.clear();
	memset(m_comp->brkAdrMap, 0, sizeof(m_comp->brkAdrMap));
	memset(m_comp->brkIOMap, 0, sizeof(m_comp->brkIOMap));
	m_comp->flgBRK = 0;
	m_comp->flgBRKMEM = 0;
}

void Machine::setIoBreak(int port, unsigned char flags) {
	// The core looks the port up by all 16 bits (spectrum.c iord/iowr), and
	// what is on the top half is whatever the instruction put there - A for
	// `out (n),a`, B for `out (c),r`. A port named by its low byte has to be
	// armed under every high byte to mean what the hardware decodes.
	if (port >= 0 && port <= 0xff) {
		for (int hi = 0; hi < 0x100; hi++)
			m_comp->brkIOMap[(hi << 8) | port] = flags;
		return;
	}
	m_comp->brkIOMap[port & 0xffff] = flags;
}

} // namespace xsp
