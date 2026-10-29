// The Beta Disk interface seen from the debugger: what the WD1793 (KR1818VG93)
// is doing, what it did, and the disk under its head.
//
// The core keeps every fact this needs - the controller's registers and the
// step of the command it is running (fdc.h), the head's position in the track
// to the byte and what each byte of the track is (floppy.h) - but no history
// and no way to stop on an event. Nothing here changes the core: a monitor is
// called around every instruction (Machine::execOne) and works out what
// happened from the difference.
//
// A disk only turns inside a controller command: every byte the controller
// steps past moves flp->pos by one, and once the motor has coasted its 15
// revolutions after a command the plan is NULL and the disk stands still. The
// angle of the disk is therefore flp->pos, and it means "where the next command
// starts", not a clock.
#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "spectrum.h"
}

namespace xsp {
namespace fdc {

// ---------------------------------------------------------------- commands

// The WD1793 command classes, decoded the way wd1793.c:vgComTab does.
enum class Kind {
	None,		// a value that matches no class: the core runs its stop plan
	Restore, Seek, Step, StepIn, StepOut,			// type I
	ReadSector, WriteSector,				// type II
	ReadAddress, ReadTrack, WriteTrack,			// type III
	Force							// type IV, $D0..$DF
};

Kind kindOf(int com);
const char* kindName(Kind k);
// "read_sector" etc. back to the class, for a filter given by name; None when
// the text names none
Kind kindFromName(const std::string& s);
bool isTypeOne(Kind k);
bool readsData(Kind k);		// the CPU takes bytes from the data register
bool writesData(Kind k);	// the CPU supplies them

// Where a type II/III command is, from its class and the step of its plan.
// Mirrors the plan tables in wd1793.c (vgRdSec, vgWrSec, vgRdAdr, vgRdTrk,
// vgWrTrk) - a core that reorders them breaks the phase checks in smoke.sh.
enum class Phase { Idle, Prepare, Seek, FindId, FindData, Transfer, Crc, Stop };
Phase phaseOf(Kind k, int planStep, bool busy);
const char* phaseName(Phase p);

// The status register as the CPU would read it now, without reading it:
// wd1793.c:vgRead composes the type I bits into fdc->state as a side effect,
// so calling it from a tool would change what the program reads next.
int statusAsRead(const FDC* fdc);
// The bits of a status value by name, for the class of the command that set it.
std::vector<std::string> statusFlags(int status, Kind k);

// ---------------------------------------------------------------- disk layout

struct SectorLayout {
	int c = 0, h = 0, s = 0, n = 0;		// the ID field: cylinder, head, sector, size code
	int idAt = -1;			// byte offset of the cylinder byte of the ID
	int dataAt = -1;		// offset of the first data byte, -1: no data field found
	int size = 0;			// 128 << (n & 3)
	int mark = 0;			// $FB data, $F8 deleted, 0 none
	bool idCrcOk = false, dataCrcOk = false;
};

struct TrackLayout {
	int trklen = 0;
	std::vector<SectorLayout> sectors;	// in the order they pass the head
};

// One side of one cylinder of a drive, from the bytes the controller reads.
TrackLayout trackLayout(const Floppy* flp, int cyl, int side);
// What is under the head at `pos` on that track: "gap", "id", "data", "crc"
// and the sector it belongs to (index into layout.sectors, -1 in a gap).
std::string fieldAt(const Floppy* flp, int cyl, int side, int pos, const TrackLayout& lay, int* sector);

// The CRC the controller computes over a field: A1 A1 A1, the mark, the bytes.
unsigned short fieldCrc(const unsigned char* markAndBytes, int len);

// ---------------------------------------------------------------- TR-DOS

// A ROM entry point by name. Every one carries the bytes it starts with, and a
// name is only given where those bytes are there: a different TR-DOS gets no
// name rather than a wrong one.
struct RomEntry {
	int address;
	const char* name;
	const char* signature;		// hex
	const char* what;
};
const std::vector<RomEntry>& trdosRom();
// The name of `adr` if TR-DOS is paged in right now and the bytes match.
const char* trdosNameAt(Computer* c, int adr);
// trdos.<name> -> address, whatever is paged in
bool trdosFind(const std::string& name, int& adr);

struct SysVar {
	int address;
	int size;		// 1, 2 or a byte count for a block
	const char* name;
	const char* what;
};
const std::vector<SysVar>& trdosVars();

// ---------------------------------------------------------------- the monitor

struct SectorRec {
	int c = -1, h = -1, s = -1, n = -1;	// ID as found; -1 before it is
	long long foundT = -1;		// T from the command start to the ID
	long long dataT = -1;		// T from the command start to the first data byte
	long long endT = -1;		// T from the command start to the last one
	int delivered = 0;		// bytes the controller put in the data register
	int lost = 0;			// of those, ones the CPU had not taken in time
	int firstLost = -1;		// byte offset in the sector of the first lost one
	int interrupts = 0;		// accepted while this sector was being transferred
	long long isrMaxT = 0;		// the longest of their handlers
	bool crcError = false;
};

struct Row {
	long long n = 0;
	enum Type { Command, Sys, Ignored } type = Command;
	long long t = 0;		// monitor T when it happened
	int frame = 0, frameT = 0;
	int pc = -1;			// the instruction that wrote the port; -1 for a reset
	std::string pcName;		// trdos.* when that is where it was
	int value = 0;			// the command or the #FF value
	Kind kind = Kind::None;
	int drive = 0, side = 0;
	int trk = 0, sec = 0, data = 0;	// registers at the start
	int head = 0;			// physical cylinder under the head at the start
	int startPos = 0, trklen = 0;	// disk angle at the start, in bytes
	long long durationT = -1;	// -1 while running
	int status = 0;			// as the CPU would read it at the end
	std::string end;		// done | forced | reset | "" while running
	std::string why;		// Ignored: busy | reset
	int revolutions = 0;
	bool fast = false;		// the fdcturbo setting was on: byte counts are approximate
	long long spinupNs = 0;		// added by the drive setup when the motor came on
	std::vector<SectorRec> sectors;
};

struct Summary {
	long long sinceT = 0;
	long long firstT = -1, lastT = -1;	// first command start, last command end
	std::map<std::string, long long> byKind;
	long long commands = 0, ignored = 0;
	long long sectors = 0, sectorsOk = 0, sectorsLost = 0;
	long long crcErrors = 0, notFound = 0;
	long long busyT = 0, seekT = 0, findT = 0, transferT = 0;
	long long revolutions = 0;
	long long delivered = 0, lost = 0, taken = 0, polls = 0;
	long long okBytes = 0;			// data of sectors read with no loss and no CRC error
	long long interrupts = 0, interruptsInTransfer = 0, sectorsHitByIsr = 0;
	long long isrMaxT = 0;
};

// What stops a run. Each one is "after the instruction it happened on".
struct Breaks {
	bool command = false;		// any command starts (or only `kind`, when set)
	Kind kind = Kind::None;
	bool end = false, error = false, lost = false, crc = false, notFound = false;
	bool index = false, intInTransfer = false;
	bool id = false;		// this ID passes the head
	int idC = 0, idH = 0, idS = 0;
	bool any() const {
		return command || end || error || lost || crc || notFound || index || intInTransfer || id;
	}
	std::vector<std::string> names() const;
};

// Per drive: what the drive setup changes about the drive.
struct DriveTweak {
	double rpmPercent = 0.0;	// faster positive; scales the controller's byte time
	int spinupMs = 0;		// added to the command when the motor comes on
};

class Monitor {
public:
	// True when the next instruction cannot touch the controller: it stands
	// still, and its ports are not even decoded with TR-DOS paged out. Asked
	// inline on every instruction, so the common case costs three loads.
	bool idle(const Computer* c) const {
		return !c->dif || c->dif->type != DIF_BDI ||
		       (!c->dif->fdc->plan && !c->flgDOS && !m_isr.open);
	}
	// Around every compExec that is not idle(). `intDue` is
	// Machine::interruptDue() asked before.
	void before(Computer* c, bool intDue);
	void after(Computer* c, bool intDue);
	// Fold the core's T counter into the monitor's own. Lazily, because the
	// idle path does not count: called at least once a frame by the loops,
	// well inside the 2^31 T an int tickCount holds. A counter that went
	// backwards was put there by a state load, which is not time passing.
	void sync(const Computer* c) {
		if (m_synced) {
			const int d = (int)((unsigned)c->tickCount - (unsigned)m_tick);
			if (d > 0) m_T += d;
		}
		m_tick = c->tickCount;
		m_synced = true;
	}
	// A reset, a snapshot load or a checkpoint restore: whatever was running
	// is history, and the next instruction must not be read as continuing it.
	void discontinuity(Computer* c, const char* why);

	// the log
	const std::deque<Row>& rows() const { return m_rows; }
	const Summary& summary() const { return m_sum; }
	long long now(const Computer* c) { sync(c); return m_T; }
	void clear(const Computer* c);
	static const size_t kRows = 1024;

	// stops
	Breaks& breaks() { return m_breaks; }
	bool stopPending() const { return !m_stop.empty(); }
	std::string takeStop() { std::string s; s.swap(m_stop); return s; }

	// drive setup
	DriveTweak& tweak(int drv) { return m_tweak[drv & 3]; }
	const DriveTweak& tweak(int drv) const { return m_tweak[drv & 3]; }

	// fdc_sweep runs the same stretch many times and must leave the log as it
	// was; it takes the whole state and puts it back
	struct Saved;
	Saved save() const;
	void restore(const Saved& s);

private:
	struct Io {
		bool out = false, in = false;
		int port = -1;		// low byte
		int val = -1;		// OUT only
	};
	static Io decode(Computer* c);
	static int driveIndex(const FDC* fdc);

	struct Snap {
		bool valid = false;
		fdcCall* plan = nullptr;
		int step = 0, idle = 1, com = 0, drq = 0, cnt = 0, state = 0, trk = 0, sec = 0;
		const Floppy* flp = nullptr;
		int flpPos = 0, motor = 0, side = 0;
		bool busy() const { return plan && !idle; }
	};
	Snap snap(Computer* c) const;

	Row* open();				// the command row still running, if any
	void startCommand(Computer* c, int pc, int value);
	void endCommand(Computer* c, const char* how);
	void push(Row&& r);
	void stop(const std::string& why);
	void closeIsr(long long now);

	std::deque<Row> m_rows;
	long long m_next = 1;
	long long m_openN = 0;		// row number of the running command, 0 = none
	Summary m_sum;
	Breaks m_breaks;
	DriveTweak m_tweak[4];
	std::string m_stop;

	long long m_T = 0;		// T since the monitor was created
	int m_tick = 0;			// tickCount m_T was last brought up to
	bool m_synced = false;
	int m_t0 = 0;			// tickCount before the step
	bool m_quick = true;		// before() decided this step cannot touch the controller
	Snap m_prev;
	Io m_io;
	int m_pcBefore = 0, m_spBefore = 0;
	int m_lastSys = -1;		// last value written to #FF
	int m_baseDelay = 0;		// bytedelay before a tweak changed it, 0: untouched

	struct Isr {
		bool open = false;
		long long startT = 0;
		int pc = 0, sp = 0;
		long long rowN = 0;	// the command it interrupted, 0 = none
		int sector = -1;	// and the sector, if one was being transferred
		bool inTransfer = false;
	} m_isr;
};

struct Monitor::Saved {
	std::deque<Row> rows;
	long long next = 1, openN = 0;
	Summary sum;
	long long T = 0;
	int lastSys = -1;
	Isr isr;
};

// ---------------------------------------------------------------- faults

// A track as it was before the first fault was put on it, so clear_faults can
// give it back byte for byte.
struct Fault {
	int drive, cyl, side, sector;
	std::string kind;		// crc_data | crc_id | no_id | no_data | deleted
};

class Faults {
public:
	// false with err when the sector is not on the track or the kind is unknown
	bool add(Floppy* flp, int drive, int cyl, int side, int sector, const std::string& kind,
		 std::string& err);
	// every fault on that drive (drive < 0: all drives) undone
	int clear(Floppy* const* flps, int drive);
	// drop the record without touching the disk: the image was replaced
	void forget(int drive);
	std::vector<Fault> list(int drive = -1) const;
	std::string kindAt(int drive, int cyl, int side, int sector) const;
	static const char* kinds();
private:
	struct Saved {
		std::vector<unsigned char> bytes, fields;
		int map[256];
	};
	std::map<int, Saved> m_orig;		// key: drive << 16 | track
	std::vector<Fault> m_list;
};

} // namespace fdc
} // namespace xsp
