// Headless Xpeccy machine: everything xgui/xcore normally does around the
// emulator core (pick hardware, load the romset, size the RAM, set up video)
// minus Qt, minus a window, minus real time.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "xsp_audio.h"
#include "xsp_config.h"
#include "xsp_labels.h"
#include "xsp_listing.h"

extern "C" {
#include "spectrum.h"
}

namespace xsp {

// Instruction/T-state accounting, filled by the execution loop while it is on.
struct Profile {
	struct Range {
		std::string tag;
		// half-open [start, end): naming a region by two labels is the normal
		// way to use this, and "up to the next label" must not charge that
		// label's first instruction - nor count entering it as a call
		int start = 0, end = 0;
		long long instructions = 0;
		long long ns = 0;
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
		long long inclusiveNs = 0;
		long long inclusiveInstructions = 0;
	};
	bool on = false;
	bool armed = false;		// a start has succeeded at least once
	long long instructions = 0;
	long long ns = 0;
	long long frames = 0;
	long long otherInstructions = 0;	// what matched no range at all
	long long otherNs = 0;
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

struct RunResult {
	std::string reason;		// steps|breakpoint|pc|frames|halt|budget
	int pc = 0;
	long long instructions = 0;
	long long ns = 0;		// compExec() returns nanoseconds, not T-states
	int frames = 0;
	int brk_type = 0;		// BRK_* (libxpeccy/defines.h) when reason=breakpoint
	int brk_addr = -1;
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
	long long work_ns = 0;
	long long idle_ns = 0;
	int interrupts = 0;		// hardware frames crossed
	long long instructions = 0;
	bool complete = false;		// a boundary was actually reached
};

class Machine {
public:
	~Machine();

	// create the machine and apply the emulator's own defaults; configDir
	// overrides where Xpeccy's configuration is looked for
	bool init(std::string& err, const std::string& configDir = "");

	bool setModel(const std::string& name, std::string& err);
	bool setMemory(int kb, std::string& err);
	bool setRomset(const std::string& name, std::string& err);
	bool setLayout(const std::string& name, std::string& err);
	void reset(int mode = RES_DEFAULT);

	// execution
	RunResult run(long long maxInstructions, int stopPc, int maxFrames);
	RunResult runFrames(int count);
	RunResult step(int count);
	// Both take a budget because neither has a guaranteed stop: a CALL into
	// code that never returns, or a step_out with a stack that never unwinds,
	// otherwise runs until the process is killed.
	RunResult stepOver(long long maxInstructions);
	RunResult stepOut(long long maxInstructions);
	RunResult stepLine();			// step until the source line changes

	// memory
	int readByte(int adr) const;
	void writeByte(int adr, int val);
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
	void setBreakBank(int bank, int offset, unsigned char flags);
	void setBreakAddress(int adr, unsigned char flags);
	unsigned char getBreak(int adr) const;
	void clearBreaks();
	void setIoBreak(int port, unsigned char flags);
	int physAddress(int adr) const;		// where a CPU address lands right now
	int bankAt(int adr) const;		// which page is mapped there right now

	Computer* comp() const { return m_comp; }
	Env& env() { return m_env; }
	Labels& labels() { return m_labels; }
	Listing& listing() { return m_listing; }
	Coverage& coverage() { return m_coverage; }
	Profile& profile() { return m_profile; }
	audio::Capture& audio() { return m_audio; }
	Trace& trace() { return m_trace; }
	const std::string& model() const { return m_model; }
	const std::string& romset() const { return m_romset; }
	const std::string& layout() const { return m_layout; }
	int memoryKb() const { return m_memoryKb; }
	std::vector<std::string> models() const;

private:
	void applyPalette();
	void applyLayout(const Layout& lay);
	bool loadRomset(const Romset& rs, std::string& err);
	RunResult execLoop(long long maxInstructions, int stopPc, int maxFrames, int stopSp);
	// profiler + trace, per instruction. `sp` is SP as it was *before* the
	// instruction ran: compExec() has already executed it by the time we get
	// here, and the value afterwards cannot tell a routine returning from its
	// caller pushing again - both land on the same number.
	void account(int pc, int sp, long long ns);
	void coverPc(int pcBefore, int pcAfter);
	bool isReturnAt(int adr) const;		// is the instruction there a ret?

	// one armed cell, remembered physically so that clearing it finds the same
	// cell even if the paging has moved since
	struct PhysBreak { int type; int abs; };
	unsigned char* brkCell(int type, int abs) const;
	void armCell(int type, int abs, unsigned char flags);

	Computer* m_comp = nullptr;
	CPU* m_timingCpu = nullptr;	// sandbox for instrTiming(), created on demand
	Env m_env;
	Labels m_labels;
	Listing m_listing;
	Coverage m_coverage;
	Profile m_profile;
	Trace m_trace;
	audio::Capture m_audio;
	std::string m_model, m_romset, m_layout;
	int m_memoryKb = 128;
	std::vector<PhysBreak> m_breakList;	// cells we armed, so clearBreaks() can undo them
};

// number parsing shared by the tools: 4660, "0x1234", "$1234", "#1234", "%1010"
bool parseNumber(const std::string& s, int& out);

} // namespace xsp
