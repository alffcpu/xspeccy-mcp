#include "xsp_machine.h"
#include "xsp_platform.h"
#include "xsp_video.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

extern "C" {
#include "hardware/hardware.h"
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
	ns = 0;
	frames = 0;
	otherInstructions = 0;
	otherNs = 0;
	prevPc = -1;
	memset(page, 0, sizeof(page));
	stack.clear();
	for (auto& r : ranges) {
		r.instructions = 0;
		r.ns = 0;
		r.calls = 0;
		r.entries = 0;
		r.startHits = 0;
		r.inside = false;
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

// An instruction covers the bytes between the PC before and after it, unless it
// jumped - then all we can honestly claim is the opcode's first byte.
void Machine::coverPc(int pcBefore, int pcAfter) {
	if (!m_coverage.on) return;
	m_coverage.instructions++;
	int delta = (pcAfter - pcBefore) & 0xffff;
	if (delta >= 1 && delta <= 4) m_coverage.mark(pcBefore, pcBefore + delta - 1);
	else m_coverage.mark(pcBefore, pcBefore);
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

void Machine::account(int pc, int sp, long long ns) {
	if (m_profile.on) {
		m_profile.instructions++;
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
			m_profile.ranges[f.range].inclusiveNs += ns;
			m_profile.ranges[f.range].inclusiveInstructions++;
		}

		if (!any) {
			m_profile.otherInstructions++;
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

	m_comp = compCreate();
	if (!m_comp) { err = "compCreate() failed"; return false; }

	m_memoryKb = m_env.memoryKb;
	if (!setModel(m_env.model, err)) {
		// an unknown model in the profile must not brick the server
		std::string ignored;
		if (!setModel("Pentagon", ignored)) return false;
		m_env.note += " [model '" + m_env.model + "' unknown, using Pentagon]";
	}

	std::string rerr;
	if (!setRomset(m_env.romsetName, rerr))
		m_env.note += " [romset: " + rerr + "]";

	if (!setLayout(m_env.layoutName, err)) setLayout("default", err);
	applyPalette();

	compSetBaseFrq(m_comp, m_env.cpuFrq / 1e6);
	reset(RES_DEFAULT);
	return true;
}

std::vector<std::string> Machine::models() const {
	std::vector<std::string> res;
	for (tabHwItem* it = tabHwPtr; it->id != HW_NULL; it++)
		if (it->core && it->core->name) res.push_back(it->core->name);
	return res;
}

// The disk controller a machine physically had: a Beta Disk (TR-DOS) on the Soviet ZX
// clones, the +3's own uPD765, and nothing on hardware that had no drive.
static int diskInterfaceFor(HardWare* hw) {
	if (!hw || hw->grp != HWG_ZX) return DIF_NONE;
	if (hw->id == HW_PLUS3) return DIF_P3DOS;
	if (hw->id == HW_PLUS2) return DIF_NONE;	// the grey +2 had a tape deck, no drive
	return DIF_BDI;
}

// CPU cores with IN/OUT-style port instructions. The 6502 and the PDP-11 clones address
// their hardware through memory, so they can never reach the core's port path.
static bool cpuHasPortIo(int type) {
	switch (type) {
		case CPU_Z80: case CPU_I8080: case CPU_LR35902:
		case CPU_I8086: case CPU_I80186: case CPU_I80286:
			return true;
		default:
			return false;
	}
}

// Machines the core will start and then break the session on. Both cases are upstream's, and
// both are reachable here only because there is no GUI to steer around them:
//
//   - Game Boy, NES, C64, BK, Specialist are memory-mapped and leave `out`/`in` NULL in the
//     hardware table, while iowr()/iord() call them unchecked. The CPU type comes from the
//     profile rather than the hardware (nothing in libxpeccy sets it), so these run on a Z80
//     here and the first OUT is a jump through a null pointer.
//   - PC-9801 no longer faults - it used to divide by zero in upd7220_line() - but its IO
//     handlers printf() every unhandled port straight to stdout, which is the JSON-RPC
//     stream. Five frames of it produced 671 lines of debug text in among the responses.
//     The core has ~220 such printfs; this is the only machine that reaches one by running.
//
// No tool here understands any of that hardware anyway, so say so instead of breaking.
static const char* unsupportedReason(Computer* c) {
	if (!c->hw) return NULL;
	if (c->hw->id == HW_PC9801)
		return "its IO handlers print debug text to stdout, which here is the JSON-RPC stream";
	if ((!c->hw->out || !c->hw->in) && cpuHasPortIo(c->cpu->type))
		return "the core has no IO handlers for it and would segfault on the first port access";
	return NULL;
}

bool Machine::setModel(const std::string& name, std::string& err) {
	const std::string previous = m_model;
	if (!compSetHardware(m_comp, name.c_str())) {
		err = "unknown machine '" + name + "'";
		return false;
	}
	if (const char* why = unsupportedReason(m_comp)) {
		compSetHardware(m_comp, previous.empty() ? "Pentagon" : previous.c_str());
		err = "'" + name + "' is emulated by the core but not usable here: " + why;
		return false;
	}
	m_model = m_comp->hw->name;
	// compCreate() leaves the machine with no disk controller at all (DIF_NONE), because
	// upstream expects the GUI to set one from the profile. Without it a reset into TR-DOS
	// runs a ROM whose drive never answers, so give every ZX machine the interface its
	// real hardware came with.
	difSetHW(m_comp->dif, diskInterfaceFor(m_comp->hw));
	// clamp the RAM size to what this hardware supports (xcore/profiles.cpp:535-541)
	std::string ignored;
	setMemory(m_memoryKb, ignored);
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
	int size = toPowerOfTwo(kb * 1024);
	if (size < MEM_256) size = MEM_256;
	if (size > MEM_4M) size = MEM_4M;
	int mask = m_comp->hw ? m_comp->hw->mask : 0;
	if (mask && (~mask & size)) {			// not supported: take the largest that is
		size = MEM_4M;
		while (size && !(mask & size)) size >>= 1;
		if (!size) { err = "hardware supports no known RAM size"; return false; }
	}
	memSetSize(m_comp->mem, size, -1);
	m_memoryKb = size / 1024;
	return true;
}

// mirrors xcore/profiles.cpp:prfSetRomset()
bool Machine::loadRomset(const Romset& rs, std::string& err) {
	memset(m_comp->mem->romData, 0xff, MEM_512K);
	int romsz = MEM_256;
	int loaded = 0;
	for (const auto& rf : rs.roms) {
		// the bytes come from a file in the romset directory, or - for the ROM
		// compiled into this binary - from the image we are already holding
		std::vector<unsigned char> src;
		if (rf.data) {
			src.assign(rf.data, rf.data + rf.dataSize);
		} else {
			std::string path = platform::join(m_env.romDir, rf.name);
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

		int foff = rf.foffset * 1024;
		int roff = rf.roffset * 1024;
		int fsze = rf.fsize > 0 ? rf.fsize * 1024 : (int)src.size();
		if (roff + fsze > romsz) {
			romsz = roff + fsze;
			if (romsz < MEM_256) romsz = MEM_256;
			if (romsz > MEM_512K) romsz = MEM_512K;
			romsz = toPowerOfTwo(romsz);
		}
		if (roff + fsze > romsz) fsze = romsz - roff;
		if (foff < 0 || foff >= (int)src.size()) continue;
		if (foff + fsze > (int)src.size()) fsze = (int)src.size() - foff;
		if (roff >= 0 && roff < MEM_512K && fsze > 0) {
			memcpy(m_comp->mem->romData + roff, src.data() + foff, fsze);
			loaded++;
		}
	}
	memSetSize(m_comp->mem, -1, romsz);
	if (!loaded) {
		if (err.empty()) err = "romset '" + rs.name + "' has no usable files";
		return false;
	}
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

void Machine::applyLayout(const Layout& lay) {
	vLayout vl;
	vl.full.x = lay.full_x;   vl.full.y = lay.full_y;
	vl.bord.x = lay.bord_x;   vl.bord.y = lay.bord_y;
	vl.blank.x = lay.blank_x; vl.blank.y = lay.blank_y;
	vl.scr.x = lay.scr_x;     vl.scr.y = lay.scr_y;
	vl.intpos.x = lay.int_x;  vl.intpos.y = lay.int_y;
	vl.intSize = lay.int_size;
	m_comp->vid->brdsize = m_env.borderSize;
	comp_set_layout(m_comp, &vl);		// uses hw->lay when the hardware fixes one
	vid_upd_layout(m_comp->vid);		// re-apply with our brdsize
	video::applyGeometry(m_comp);		// globals xgui would set (bytesPerLine etc.)
}

bool Machine::setLayout(const std::string& name, std::string& err) {
	const Layout* lay = m_env.findLayout(name);
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

void Machine::reset(int mode) {
	compReset(m_comp, mode);
	// compCreate/compReset leave the key matrix all-zero, which reads as "every
	// key held down" - the GUI clears it via comp_kbd_release() and so must we.
	kbdReleaseAll(m_comp->keyb);
	m_comp->flgBRK = 0;
}

// ---------------------------------------------------------------- execution

RunResult Machine::execLoop(long long maxInstructions, int stopPc, int maxFrames, int stopSp) {
	RunResult r;
	Computer* c = m_comp;
	c->flgBRK = 0;
	const int savedDebug = c->flgDBG;
	bool first = true;

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
		const long long ns = compExec(c);
		c->flgDBG = savedDebug;
		first = false;
		r.ns += ns;
		r.instructions++;
		account(pcBefore, spBefore, ns);
		coverPc(pcBefore, cpu_get_pc(c->cpu));

		if (c->flgFRM) {
			c->flgFRM = 0;
			r.frames++;
			if (m_profile.on) m_profile.frames++;
		}
		if (c->flgBRK) {
			r.reason = "breakpoint";
			r.brk_type = c->brkt;
			r.brk_addr = c->brka;
			c->flgBRK = 0;
			break;
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
	return r;
}

RunResult Machine::run(long long maxInstructions, int stopPc, int maxFrames) {
	return execLoop(maxInstructions, stopPc, maxFrames, -1);
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
	for (int i = 0; i < count; i++) {
		const int pcBefore = cpu_get_pc(c->cpu);
		const int spBefore = cpu_get_sp(c->cpu);
		const long long ns = compExec(c);
		r.ns += ns;
		r.instructions++;
		account(pcBefore, spBefore, ns);
		coverPc(pcBefore, cpu_get_pc(c->cpu));
		if (c->flgFRM) {
			c->flgFRM = 0;
			r.frames++;
			if (m_profile.on) m_profile.frames++;
		}
	}
	c->flgDBG = savedDebug;
	c->flgBRK = 0;
	r.pc = cpu_get_pc(c->cpu);
	r.reason = "steps";
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
		const long long ns = compExec(c);
		const int haltedAfter = c->cpu->flgHALT;
		fc.instructions++;
		// the profiler, trace and coverage see this exactly as they see a run,
		// so one pass can measure the frame and profile it at the same time
		if (instrument) {
			account(pcBefore, spBefore, ns);
			coverPc(pcBefore, cpu_get_pc(c->cpu));
		}
		// parked in HALT and still parked: pure waiting. Leaving HALT is the
		// interrupt being taken, which is real work and belongs to this frame.
		if (haltedBefore && haltedAfter) fc.idle_ns += ns;
		else fc.work_ns += ns;
		if (c->flgFRM) {
			c->flgFRM = 0;
			fc.interrupts++;
			if (instrument && m_profile.on) m_profile.frames++;
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
unsigned char* Machine::brkCell(int type, int abs) const {
	switch (type) {
		case MEM_RAM: return m_comp->brkRamMap + (abs & m_comp->mem->ramMask);
		case MEM_ROM: return m_comp->brkRomMap + (abs & m_comp->mem->romMask);
	}
	return nullptr;
}

void Machine::armCell(int type, int abs, unsigned char flags) {
	unsigned char* cell = brkCell(type, abs);
	if (!cell) return;
	*cell = (unsigned char)((*cell & 0xf0) | (flags & 0x0f));
	for (const auto& b : m_breakList)
		if (b.type == type && b.abs == abs) return;
	m_breakList.push_back(PhysBreak{type, abs});
}

void Machine::setBreak(int adr, unsigned char flags) {
	xAdr xa = mem_get_xadr(m_comp->mem, adr & 0xffff);
	armCell(xa.type, xa.abs, flags);
}

void Machine::setBreakBank(int bank, int offset, unsigned char flags) {
	// A symbol file counts in 16K banks. The core's own page number is in units
	// of its page size - 256 bytes, which is why machine_state shows bank 2 as
	// 128 - so go through the absolute address rather than the page number.
	armCell(MEM_RAM, bank * 0x4000 + (offset & 0x3fff), flags);
}

void Machine::setBreakAddress(int adr, unsigned char flags) {
	unsigned char& cell = m_comp->brkAdrMap[adr & 0xffff];
	cell = (unsigned char)((cell & 0xf0) | (flags & 0x0f));
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
		if (unsigned char* cell = brkCell(b.type, b.abs))
			*cell &= 0xf0;
	}
	m_breakList.clear();
	memset(m_comp->brkAdrMap, 0, sizeof(m_comp->brkAdrMap));
	memset(m_comp->brkIOMap, 0, sizeof(m_comp->brkIOMap));
	m_comp->flgBRK = 0;
}

void Machine::setIoBreak(int port, unsigned char flags) {
	m_comp->brkIOMap[port & 0xffff] = flags;
}

} // namespace xsp
