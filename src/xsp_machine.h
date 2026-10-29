// Headless Xpeccy machine: everything xgui/xcore normally does around the
// emulator core (pick hardware, load the romset, size the RAM, set up video)
// minus Qt, minus a window, minus real time.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xsp_audio.h"
#include "xsp_config.h"
#include "xsp_fdc.h"
#include "xsp_labels.h"
#include "xsp_listing.h"

extern "C" {
#include "spectrum.h"
}

namespace xsp {

// Instruction/T-state accounting, filled by the execution loop while it is on.
// Times are T-states, not nanoseconds: the fork's cpu counts them exactly
// (waits included) into comp->tickCount, and converting ns back through
// nsPerTick was a place the numbers could silently drift.
struct Profile {
	struct Range {
		std::string tag;
		// half-open [start, end): naming a region by two labels is the normal
		// way to use this, and "up to the next label" must not charge that
		// label's first instruction - nor count entering it as a call
		int start = 0, end = 0;
		long long instructions = 0;
		long long t = 0;
		long long ns = 0;	// real nanoseconds, accumulated like t
		// entries counts every arrival from outside; calls leaves out the ones
		// that are a nested callee returning, which is what makes calls the
		// number of times the routine was actually invoked
		long long calls = 0;
		long long entries = 0;
		long long startHits = 0;	// times execution began at `start` itself
		bool inside = false;		// was the previous instruction in here
		// self time is what ran with PC in here; inclusive adds everything the
		// range called out to, which is the number that answers "what does this
		// routine cost me per frame"
		long long inclusiveT = 0;
		long long inclusiveNs = 0;
		long long inclusiveInstructions = 0;
	};
	bool on = false;
	bool armed = false;		// a start has succeeded at least once
	long long instructions = 0;
	long long t = 0;
	long long ns = 0;	// real nanoseconds across frequency changes
	long long frames = 0;
	long long otherInstructions = 0;	// what matched no range at all
	long long otherT = 0;
	long long otherNs = 0;
	// Interrupts the CPU took. Accepting one is 11-19 T of bus cycles that run
	// no instruction at any address: they are in `t` (the frame really spent
	// them) but in no range's self time and not in `other`, so ranges + other
	// + interrupt adds up to the total.
	long long interrupts = 0;
	long long interruptT = 0;
	long long interruptNs = 0;
	int prevPc = -1;		// address of the previously executed instruction
	long long page[256] = {0};	// instructions per 256-byte page of the address space
	std::vector<Range> ranges;

	// Invocations still running, innermost last. An entry is dropped once SP has
	// risen past what it was when the routine started, which is the cheapest
	// honest answer to "has it returned" that does not depend on how it returned.
	struct Frame { int range; int sp; };
	std::vector<Frame> stack;

	void reset();
};

// Which bytes of the address space the CPU actually touched.
struct Coverage {
	bool on = false;
	long long instructions = 0;
	unsigned char map[0x10000] = {0};	// bit 0: executed

	void reset();
	void mark(int from, int to);		// inclusive byte range
};

// Ring buffer of executed addresses - what ran just before things went wrong.
struct Trace {
	bool on = false;
	size_t capacity = 4096;
	std::vector<int> pcs;
	size_t pos = 0;
	bool wrapped = false;

	void reset(size_t cap);
	void add(int pc);
	std::vector<int> dump(size_t count) const;	// oldest first
};

// Where the beam was when a watched address executed.
//
// One entry on its own says little; the point is the spread across frames. A
// raster routine that is correct reaches the same address at the same T-state
// every frame, and one that is not shows up here immediately as jitter, which
// no amount of hashing the screen memory can reveal.
struct BeamLog {
	struct Event {
		int pc = 0;
		int frame = 0;		// the video frame counter at the time
		int t = 0;		// T-states since this frame's interrupt
		int line = 0, dot = 0;	// beam position
	};
	bool on = false;
	size_t capacity = 4096;
	std::vector<Event> events;
	size_t pos = 0;
	bool wrapped = false;
	long long total = 0;			// events seen, including overwritten ones
	unsigned char watch[0x10000] = {0};	// 1: log execution of this address

	void reset(size_t cap);
	void clearWatch();
	void addWatch(int adr) { watch[adr & 0xffff] = 1; }
	bool watching(int adr) const { return watch[adr & 0xffff] != 0; }
	void add(const Event& e);
	std::vector<Event> dump(size_t count) const;	// oldest first
	std::vector<int> watched() const;
};

struct RunResult {
	std::string reason;		// steps|breakpoint|pc|frames|halt|budget|beam|fdc
	std::string fdc;		// reason=fdc: the controller event that stopped it
	int pc = 0;
	long long instructions = 0;
	// interrupts accepted on the way: each is a step that ran no instruction,
	// so it is in t and ns but not in instructions
	long long interrupts = 0;
	long long t = 0;		// T-states, from the tickCount delta per instruction
	// nanoseconds, accumulated the way the core charged them: t * nsPerTick
	// once per instruction. Summing it at the end with the final nsPerTick
	// reports a turbo change mid-run at the wrong rate for everything before it.
	long long ns = 0;
	int frames = 0;
	int brk_type = 0;		// BRK_* (libxpeccy/defines.h) when reason=breakpoint
	int brk_addr = -1;
	// Where the beam stood when execution stopped. Carried on every stop so
	// that raster work does not need a second call to find out, which is half
	// the questions asked while debugging multicolour.
	int beam_line = -1, beam_dot = -1;
	int beam_t = -1;		// T-states since the interrupt
	int beam_frame = -1;
};

struct AsmLine {
	std::string text;
	int address = 0;
	std::vector<unsigned char> bytes;
	bool ok = false;
	std::string error;
};

// What one instruction costs, measured by running it in a throwaway CPU rather
// than looked up in a hand-written table. `taken` differs from `not_taken` only
// for conditional jumps, calls, returns and djnz.
struct InstrTiming {
	int taken = 0;
	int not_taken = 0;
	bool conditional = false;	// the two differ
};

// One effect frame: the work between two frame boundaries, with the time the
// CPU spent parked in HALT waiting for an interrupt kept separate. Adding them
// gives the wall-clock length of the frame, which is why headroom == idle.
struct FrameCost {
	long long work_t = 0;
	long long idle_t = 0;
	int interrupts = 0;		// hardware frames crossed
	long long instructions = 0;
	bool complete = false;		// a boundary was actually reached
};

class Machine {
public:
	Machine() = default;
	~Machine();

	// Owns a Computer and a CPU and frees both by hand, so a copy would free
	// them twice. There is one machine per process and no reason to move one,
	// which makes deleting the copy the whole answer rather than the first half
	// of writing five functions nobody calls.
	Machine(const Machine&) = delete;
	Machine& operator=(const Machine&) = delete;

	// create the machine and apply the emulator's own defaults; configDir
	// overrides where Xpeccy's configuration is looked for
	bool init(std::string& err, const std::string& configDir = "");

	bool setModel(const std::string& name, std::string& err);
	bool setMemory(int kb, std::string& err);
	bool setRomset(const std::string& name, std::string& err);
	bool setLayout(const std::string& name, std::string& err);
	// How much of the border the frame carries, 0..1 - Xpeccy's `bordersize`.
	// Changing it changes the size of every picture the tools produce.
	double borderSize() const;
	void setBorderSize(double frac);
	void reset(int mode = RES_DEFAULT);

	// The geometry a mounted disk image was formatted with, per drive. Every
	// path through zx_init (compReset, compSetTurbo/compSetBaseFrq, a port
	// write that flips turbo mid-run, the snapshot loaders' own compReset)
	// shrinks them all to DD; reapplyDriveGeom() puts them back. Record the
	// live flp->trklen on every successful insert, 0 on eject.
	void noteDriveGeom(int drv, long trklen);
	void reapplyDriveGeom();

	// execution
	RunResult run(long long maxInstructions, int stopPc, int maxFrames);
	RunResult runFrames(int count);
	RunResult step(int count);
	// Run until the beam reaches a raster position, wrapping into the next
	// frame when the target is already behind. `dot` < 0 means "anywhere on
	// that line". Stops on the first instruction that reached or passed it.
	RunResult runToBeam(int line, int dot, long long maxInstructions);
	// Both take a budget because neither has a guaranteed stop: a CALL into
	// code that never returns, or a step_out with a stack that never unwinds,
	// otherwise runs until the process is killed.
	RunResult stepOver(long long maxInstructions);
	RunResult stepOut(long long maxInstructions);
	RunResult stepLine();			// step until the source line changes

	// memory
	int readByte(int adr) const;
	void writeByte(int adr, int val);
	// a physical 16K RAM bank, paged in or not - the same bank:offset unit a
	// symbol file uses. -1 when the machine has no such bank, or `off` is not
	// inside one ($0000..$3FFF).
	int readBankByte(int bank, int off) const;
	int bankCount() const;			// ramSize / 16K
	int findBytes(const std::vector<unsigned char>& pat, int from, int to) const;

	// code
	std::string disasm(int adr, int* len, xMnem* mn) const;
	// `defined`, when given, receives the labels this block declared itself
	std::vector<AsmLine> assemble(int adr, const std::vector<std::string>& lines, bool write,
				      std::map<std::string, int>* defined = nullptr);
	InstrTiming instrTiming(int adr);	// T-states of the instruction at adr

	// One effect frame. sync < 0 means "until the CPU enters HALT", otherwise
	// "until PC reaches sync". Budget is in instructions. With instrument=false
	// the profiler, trace and coverage do not see it - used to skip the partial
	// frame that gets us onto a boundary in the first place.
	FrameCost frameCost(int sync, long long maxInstructions, bool instrument = true);

	// Breakpoints (MEM_BRK_* bits from libxpeccy/cartridge.h). The core keeps
	// two kinds and checks them in this order:
	//   address - lives in the CPU address, fires whatever is paged in there
	//   cell    - lives in the memory cell, so it follows the bank
	// setBreak() arms the cell that is mapped at `adr` right now; setBreakBank()
	// arms one by page number, so it lands where a bank:offset label means even
	// if that bank is not paged in at the time.
	void setBreak(int adr, unsigned char flags);
	// false (with err) when the machine has no such bank or offset
	bool setBreakBank(int bank, int offset, unsigned char flags, std::string& err);
	void setBreakAddress(int adr, unsigned char flags);
	unsigned char getBreak(int adr) const;
	void clearBreaks();
	// A port up to $FF is the low byte the hardware decodes, so it is armed
	// for every high byte: `out ($FE),a` puts A on the top half of the bus and
	// a port break on $FE has to catch it whatever A was. Anything above $FF
	// is one exact 16-bit port.
	void setIoBreak(int port, unsigned char flags);
	int physAddress(int adr) const;		// where a CPU address lands right now
	int bankAt(int adr) const;		// which page is mapped there right now

	Computer* comp() const { return m_comp; }
	fdc::Monitor& fdc() { return m_fdc; }
	fdc::Faults& faults() { return m_faults; }

	// Whole-machine states kept in this process (core xstate): the controller
	// mid-command and the angle of the disk included, which a .sna has no room
	// for. Not written to a file - the state holds pointers into the core's
	// own tables, which the next run of the server has somewhere else.
	struct Checkpoint {
		std::string name;
		std::string model, machineId;
		int memoryKb = 0;
		std::string images[4];		// what was in the drives, to warn on load
		long long savedT = 0;		// fdc monitor T at the save
		int frame = 0;
		size_t bytes = 0;
	};
	bool saveCheckpoint(const std::string& name, std::string& err);
	// warnings: what is not as it was (another image in a drive)
	bool loadCheckpoint(const std::string& name, std::string& err, std::vector<std::string>& warnings);
	bool dropCheckpoint(const std::string& name);
	std::vector<Checkpoint> checkpoints() const;
	// An unnamed one for a tool that runs and puts the machine back (fdc_sweep).
	// The Computer is still the one it was taken from, or the load fails.
	void* takeState(std::string& err);
	bool putState(void* st, const char* why);
	void freeState(void* st);
	Env& env() { return m_env; }
	Labels& labels() { return m_labels; }
	Listing& listing() { return m_listing; }
	Coverage& coverage() { return m_coverage; }
	Profile& profile() { return m_profile; }
	audio::Capture& audio() { return m_audio; }
	Trace& trace() { return m_trace; }
	BeamLog& beamLog() { return m_beamLog; }
	const std::string& model() const { return m_model; }
	// the definition stem the current machine was built from, "" when none
	const std::string& machineDefId() const { return m_machineId; }
	const std::string& romset() const { return m_romset; }
	const std::string& layout() const { return m_layout; }
	int memoryKb() const { return m_memoryKb; }
	std::vector<std::string> models() const;

private:
	void applyPalette();
	void applyLayout(const Layout& lay);
	void applyMachineDef(const MachineDef& def);	// machines/<id>.conf settings
	bool loadRomset(const Romset& rs, std::string& err);
	std::string romPath(const std::string& name) const;
	// beamLine < 0 disables the raster stop; see runToBeam().
	RunResult execLoop(long long maxInstructions, int stopPc, int maxFrames, int stopSp,
			   int beamLine = -1, int beamDot = -1);
	void stampBeam(RunResult& r) const;	// beam position into a finished result
	// One BeamLog entry for the current state. Taken before the step and kept
	// only if the step turned out to run the instruction: a fetch break stops
	// in front of it and an interrupt runs instead of it, and either way the
	// same PC comes round again on the next step.
	BeamLog::Event beamEvent(int pc) const;
	// Will the next compExec() accept an interrupt instead of running the
	// opcode at PC? Asked before the step; see the definition for why it can be.
	bool interruptDue() const;
	// a step that accepted an interrupt: its T go to the profile's interrupt
	// counters and the audio clock, nowhere that is about an instruction
	void accountInterrupt(int pcBefore, long long t, long long ns);
	// profiler + trace + audio, per instruction. `sp` is SP as it was *before*
	// the instruction ran: compExec() has already executed it by the time we
	// get here, and the value afterwards cannot tell a routine returning from
	// its caller pushing again - both land on the same number. `t` is the
	// exact T the instruction took (tickCount delta, waits included); `ns` is
	// what compExec() reported, which audio pacing prefers.
	void account(int pc, int sp, long long t, long long ns);
	// `len` is the instruction's own length, read before it ran
	void coverPc(int pcBefore, int len);
	int instrLen(int adr) const;		// bytes of the instruction at adr
	bool isReturnAt(int adr) const;		// is the instruction there a ret?

	// One armed cell, remembered by its index in the core's map - masked when
	// it was armed - so that clearing it finds the same cell even if the
	// paging, the RAM size or the rom size has moved since. Masking the stored
	// address again at clear time with whatever mask is current then would
	// clear a different cell and leave this one armed.
	struct PhysBreak { int type; int idx; };
	unsigned char* brkCell(int type, int idx) const;
	void armCell(int type, int abs, unsigned char flags);

	// one core instruction plus the bookkeeping that must follow it
	long long execOne(bool intDue);
	// after the core's state was replaced wholesale: what the core does not
	// carry has to agree with it again
	void afterStateLoad(const char* why);
	Computer* m_comp = nullptr;
	CPU* m_timingCpu = nullptr;	// sandbox for instrTiming(), created on demand
	Env m_env;
	Labels m_labels;
	Listing m_listing;
	Coverage m_coverage;
	Profile m_profile;
	Trace m_trace;
	BeamLog m_beamLog;
	audio::Capture m_audio;
	std::string m_model, m_machineId, m_romset, m_layout;
	int m_memoryKb = 128;
	double m_borderFrac = 0.5;	// asked-for border share; the core sees the nearest VID_BRD_*
	std::vector<PhysBreak> m_breakList;	// cells we armed, so clearBreaks() can undo them
	// What Env::load() and init() had to say about the configuration. The
	// notes a machine switch adds (a rom that did not load, a font that is
	// not there) belong to that machine, and each switch starts from this.
	std::string m_envNote;
	long m_driveGeom[4] = {0, 0, 0, 0};	// trklen a mounted image needs; 0 = DD
	fdc::Monitor m_fdc;
	fdc::Faults m_faults;
	struct Held;			// a checkpoint with its xstate
	std::map<std::string, std::shared_ptr<Held>> m_checkpoints;
};

// number parsing shared by the tools: 4660, "0x1234", "$1234", "#1234", "%1010"
bool parseNumber(const std::string& s, int& out);

} // namespace xsp
