#include "xsp_fdc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cpu/Z80/z80.h"	// regA/regBC/... for the port an OUT goes to

namespace xsp {
namespace fdc {

// ---------------------------------------------------------------- commands

Kind kindOf(int com) {
	com &= 0xff;
	// vgExec tests for the force interrupt before it looks at the table
	if ((com & 0xf0) == 0xd0) return Kind::Force;
	if ((com & 0xf0) == 0x00) return Kind::Restore;
	if ((com & 0xf0) == 0x10) return Kind::Seek;
	if ((com & 0xe0) == 0x20) return Kind::Step;
	if ((com & 0xe0) == 0x40) return Kind::StepIn;
	if ((com & 0xe0) == 0x60) return Kind::StepOut;
	if ((com & 0xe1) == 0x80) return Kind::ReadSector;
	if ((com & 0xe0) == 0xa0) return Kind::WriteSector;
	if ((com & 0xfb) == 0xc0) return Kind::ReadAddress;
	if ((com & 0xfb) == 0xe0) return Kind::ReadTrack;
	if ((com & 0xfb) == 0xf0) return Kind::WriteTrack;
	return Kind::None;
}

static const struct { Kind k; const char* name; } kKindNames[] = {
	{Kind::None, "none"},
	{Kind::Restore, "restore"}, {Kind::Seek, "seek"}, {Kind::Step, "step"},
	{Kind::StepIn, "step_in"}, {Kind::StepOut, "step_out"},
	{Kind::ReadSector, "read_sector"}, {Kind::WriteSector, "write_sector"},
	{Kind::ReadAddress, "read_address"}, {Kind::ReadTrack, "read_track"},
	{Kind::WriteTrack, "write_track"}, {Kind::Force, "force_interrupt"},
};

const char* kindName(Kind k) {
	for (const auto& e : kKindNames)
		if (e.k == k) return e.name;
	return "none";
}

Kind kindFromName(const std::string& s) {
	for (const auto& e : kKindNames)
		if (s == e.name) return e.k;
	return Kind::None;
}

bool isTypeOne(Kind k) {
	return k == Kind::Restore || k == Kind::Seek || k == Kind::Step ||
	       k == Kind::StepIn || k == Kind::StepOut;
}

bool readsData(Kind k) {
	return k == Kind::ReadSector || k == Kind::ReadAddress || k == Kind::ReadTrack;
}

bool writesData(Kind k) {
	return k == Kind::WriteSector || k == Kind::WriteTrack;
}

Phase phaseOf(Kind k, int step, bool busy) {
	if (!busy) return Phase::Idle;
	if (isTypeOne(k)) return Phase::Seek;
	switch (k) {
	// vgRdSec: rds00 prepare, rds01 ID, rds02 data mark, rds03 crc/size, rds04 bytes, rds05 crc
	case Kind::ReadSector:
		switch (step) {
		case 0: return Phase::Prepare;
		case 1: return Phase::FindId;
		case 2: return Phase::FindData;
		case 3: case 4: return Phase::Transfer;
		case 5: return Phase::Crc;
		default: return Phase::Stop;
		}
	// vgWrSec: rds00, wrs00 protect, rds01 ID, rds02 data mark, wrs01 mark, wrs02 bytes, wrs03 crc
	case Kind::WriteSector:
		switch (step) {
		case 0: case 1: return Phase::Prepare;
		case 2: return Phase::FindId;
		case 3: return Phase::FindData;
		case 4: case 5: return Phase::Transfer;
		case 6: return Phase::Crc;
		default: return Phase::Stop;
		}
	// vgRdAdr: rds00, rda00 ID, rda01 six bytes, chk02 crc
	case Kind::ReadAddress:
		switch (step) {
		case 0: return Phase::Prepare;
		case 1: return Phase::FindId;
		case 2: return Phase::Transfer;
		case 3: return Phase::Crc;
		default: return Phase::Stop;
		}
	// vgRdTrk: rds00, rdt00 index, rdt01 bytes
	case Kind::ReadTrack:
		switch (step) {
		case 0: return Phase::Prepare;
		case 1: return Phase::FindId;
		case 2: return Phase::Transfer;
		default: return Phase::Stop;
		}
	// vgWrTrk: rds00, wrs00 protect, wrt00 index, wrt01 bytes, wrt02 fields
	case Kind::WriteTrack:
		switch (step) {
		case 0: case 1: return Phase::Prepare;
		case 2: return Phase::FindId;
		case 3: return Phase::Transfer;
		default: return Phase::Stop;
		}
	default:
		return Phase::Stop;
	}
}

const char* phaseName(Phase p) {
	switch (p) {
	case Phase::Idle: return "idle";
	case Phase::Prepare: return "prepare";
	case Phase::Seek: return "seek";
	case Phase::FindId: return "find_id";
	case Phase::FindData: return "find_data";
	case Phase::Transfer: return "transfer";
	case Phase::Crc: return "crc";
	case Phase::Stop: return "stop";
	}
	return "idle";
}

// wd1793.c:vgRead, case FDC_COM, without the writes back into fdc->state
int statusAsRead(const FDC* fdc) {
	const Floppy* flp = fdc->flp;
	int st = fdc->state & 0x7e;
	if (!(flp->insert && flp->door)) st |= 0x80;
	if (!fdc->idle) st |= 0x01;
	if (fdc->fmode == 0) {
		st &= 0x99;
		if (flp->protect) st |= 0x40;
		if (flp->motor) st |= 0x20;
		if (flp->fitted && flp->trk == 0) st |= 0x04;
		if (flp->insert && flp->door && flp->motor && flp->index) st |= 0x02;
	} else if (fdc->fmode == 1) {
		st &= 0xfd;
		if (fdc->drq) st |= 0x02;
	}
	return st & 0xff;
}

std::vector<std::string> statusFlags(int st, Kind k) {
	static const char* typeOne[8] = {"busy", "index", "track0", "crc_error",
					 "seek_error", "head_loaded", "write_protect", "not_ready"};
	static const char* typeTwo[8] = {"busy", "drq", "lost_data", "crc_error",
					 "not_found", "deleted_mark", "write_protect", "not_ready"};
	const bool one = isTypeOne(k) || k == Kind::None || k == Kind::Force;
	std::vector<std::string> out;
	for (int b = 7; b >= 0; b--) {
		if (!(st & (1 << b))) continue;
		const char* n = one ? typeOne[b] : typeTwo[b];
		if (!one && b == 5 && writesData(k)) n = "write_fault";
		out.push_back(n);
	}
	return out;
}

// ---------------------------------------------------------------- disk layout

// wd1793.c:add_crc_16, starting where three A1 sync bytes leave it
unsigned short fieldCrc(const unsigned char* p, int len) {
	unsigned int crc = 0xcdb4;
	for (int i = 0; i < len; i++) {
		crc ^= (unsigned)p[i] << 8;
		for (int b = 0; b < 8; b++)
			if ((crc <<= 1) & 0x10000) crc ^= 0x1021;
	}
	return crc & 0xffff;
}

static int trackIndex(const Floppy* flp, int cyl, int side) {
	return (cyl << 1) | (side & 1);
}

TrackLayout trackLayout(const Floppy* flp, int cyl, int side) {
	TrackLayout lay;
	const int tr = trackIndex(flp, cyl, side);
	if (!flp || tr < 0 || tr > 255 || flp->trklen <= 0) return lay;
	const int len = flp->trklen;
	lay.trklen = len;
	const unsigned char* b = flp->data[tr].byte;
	const unsigned char* f = flp->data[tr].field;
	auto fld = [&](int i) { return f[((i % len) + len) % len] & 0x0f; };
	auto byt = [&](int i) { return b[((i % len) + len) % len]; };
	std::vector<unsigned char> buf;
	for (int i = 0; i < len; i++) {
		// an ID field is the four bytes after an $FE mark (floppy.c:flpFillFields)
		if (fld(i) != 1 || fld(i - 1) == 1) continue;
		SectorLayout s;
		s.idAt = i;
		s.c = byt(i); s.h = byt(i + 1); s.s = byt(i + 2); s.n = byt(i + 3);
		s.size = 128 << (s.n & 3);
		buf.assign({(unsigned char)byt(i - 1)});
		for (int k = 0; k < 4; k++) buf.push_back((unsigned char)byt(i + k));
		const int idCrc = (byt(i + 4) << 8) | byt(i + 5);
		s.idCrcOk = fieldCrc(buf.data(), (int)buf.size()) == idCrc;
		// the data field the controller would take: within 52 bytes of the ID's
		// end (wd1793.c:vgrds02), and before any next ID
		for (int j = i + 4; j < i + 4 + 6 + 52; j++) {
			const int fj = fld(j);
			if (fj == 1) break;
			if ((fj == 2 || fj == 3) && fld(j - 1) != fj) {
				s.dataAt = j % len;
				s.mark = byt(j - 1);
				buf.assign({(unsigned char)s.mark});
				for (int k = 0; k < s.size; k++) buf.push_back((unsigned char)byt(j + k));
				const int dCrc = (byt(j + s.size) << 8) | byt(j + s.size + 1);
				s.dataCrcOk = fieldCrc(buf.data(), (int)buf.size()) == dCrc;
				break;
			}
		}
		lay.sectors.push_back(s);
	}
	return lay;
}

std::string fieldAt(const Floppy* flp, int cyl, int side, int pos, const TrackLayout& lay, int* sector) {
	if (sector) *sector = -1;
	const int len = lay.trklen;
	if (len <= 0) return "gap";
	auto ahead = [&](int from, int to) { return ((to - from) % len + len) % len; };
	for (size_t k = 0; k < lay.sectors.size(); k++) {
		const auto& s = lay.sectors[k];
		const int intoId = ahead(s.idAt, pos);
		if (intoId < 4) { if (sector) *sector = (int)k; return "id"; }
		if (intoId < 6) { if (sector) *sector = (int)k; return "crc"; }
		if (s.dataAt >= 0) {
			const int intoData = ahead(s.dataAt, pos);
			if (intoData < s.size) { if (sector) *sector = (int)k; return "data"; }
			if (intoData < s.size + 2) { if (sector) *sector = (int)k; return "crc"; }
		}
	}
	(void)flp; (void)cyl; (void)side;
	return "gap";
}

// ---------------------------------------------------------------- TR-DOS

// Each address checked against trdos.rom (5.03) and trdos504t.rom (5.04T) as
// shipped with xpeccy-plus, and against the programandala-net disassembly of
// 5.03 where it names the routine.
const std::vector<RomEntry>& trdosRom() {
	static const std::vector<RomEntry> t = {
		{0x1e3d, "trdos.read_sectors", "af1824",
		 "body of #3D13 function 5: read B sectors from D track, E sector to HL"},
		{0x2090, "trdos.read_sector", "f33e80d31f",
		 "di, ld a,#80, out (#1F),a: read one sector by table entry, with retries"},
		{0x2093, "trdos.read_sector_cmd", "d31fc5cde53f",
		 "out (#1F),a with the command in A, then read the bytes through #3FE5"},
		{0x2a53, "trdos.out_c", "ed79c9", "out (c),a : ret - a port write from RAM"},
		{0x3d00, "trdos.entry", "00182e", "RANDOMIZE USR 15616: enter TR-DOS"},
		{0x3d03, "trdos.command", "001814", "RANDOMIZE USR 15619: one TR-DOS command from BASIC"},
		{0x3d13, "trdos.call", "0018e7", "run TR-DOS function C"},
		{0x3d21, "trdos.init", "cdf331", "prepare TR-DOS and initialise its variables"},
		{0x3d2f, "trdos.ret", "00c9",
		 "nop : ret - jump here from RAM with a ROM address on the stack to run it"},
		{0x3fca, "trdos.write_bytes", "dbffe6c028faf8eda3",
		 "wait for DRQ/INTRQ on #FF, outi to #7F until INTRQ"},
		{0x3fe5, "trdos.read_bytes", "dbffe6c028faf8eda2",
		 "wait for DRQ/INTRQ on #FF, ini from #7F until INTRQ"},
	};
	return t;
}

static bool signatureAt(Computer* c, const RomEntry& e) {
	const char* h = e.signature;
	for (int i = 0; h[0] && h[1]; i++, h += 2) {
		unsigned v = 0;
		if (sscanf(h, "%2x", &v) != 1) return false;
		if ((memRd(c->mem, (e.address + i) & 0xffff) & 0xff) != (int)v) return false;
	}
	return true;
}

const char* trdosNameAt(Computer* c, int adr) {
	if (!c || !c->flgDOS || adr < 0 || adr > 0x3fff) return nullptr;
	for (const auto& e : trdosRom())
		if (e.address == adr) return signatureAt(c, e) ? e.name : nullptr;
	return nullptr;
}

bool trdosFind(const std::string& name, int& adr) {
	for (const auto& e : trdosRom())
		if (name == e.name) { adr = e.address; return true; }
	return false;
}

// Addresses agree between Mac Buster's "TR-DOS v5.xx variables" (1999-2000) and
// the symbol file of the programandala-net 5.03 disassembly; only what both
// give the same meaning is here.
const std::vector<SysVar>& trdosVars() {
	static const std::vector<SysVar> v = {
		{0x5cc8, 4, "drive_modes", "per drive A..D, bit 7 set: 80 tracks"},
		{0x5cd6, 1, "bad_sectors", "sectors that failed; #FF when a command did not finish (also MOVE's first track)"},
		{0x5cdd, 16, "file_descriptor", "name 8, type 1, start 2, length 2, sectors 1, first sector 1, first track 1"},
		{0x5cf4, 1, "sector", "current sector"},
		{0x5cf5, 1, "track", "current track (logical, cylinder * 2 + side)"},
		{0x5cf6, 1, "temp_drive", "drive for the current operation"},
		{0x5cfa, 4, "step_rates", "per drive A..D"},
		{0x5cfe, 1, "last_command", "last FDC command TR-DOS issued; picks its read (#80) or write (#A0) loop"},
		{0x5cff, 1, "sector_arg", "sector for the sector functions (set by function 3)"},
		{0x5d00, 2, "buffer", "buffer address for functions 5 and 6 (set by function 4)"},
		{0x5d0e, 1, "mode", "#FF: called from BASIC"},
		{0x5d0f, 1, "error", "TR-DOS error code"},
		{0x5d10, 1, "error_hi", "high byte of the error code, cleared by #3D00"},
		{0x5d16, 1, "sys_copy", "copy of the last value written to #FF"},
		{0x5d19, 1, "default_drive", "default drive 0..3"},
	};
	return v;
}

// ---------------------------------------------------------------- the monitor

int Monitor::driveIndex(const FDC* fdc) {
	for (int i = 0; i < 4; i++)
		if (fdc->flop[i] == fdc->flp) return i;
	return 0;
}

Monitor::Snap Monitor::snap(Computer* c) const {
	Snap s;
	const FDC* f = c->dif->fdc;
	s.valid = true;
	s.plan = f->plan;
	s.step = f->pos;
	s.idle = f->idle;
	s.com = f->com;
	s.drq = f->drq;
	s.cnt = f->cnt;
	s.state = f->state;
	s.trk = f->trk;
	s.sec = f->sec;
	s.flp = f->flp;
	s.flpPos = f->flp ? f->flp->pos : 0;
	s.motor = f->flp ? f->flp->motor : 0;
	s.side = f->side;
	return s;
}

// The port an instruction is about to read or write, from its opcode and the
// registers before it runs. Only the low byte matters: the Beta interface
// decodes nothing else (diskif.c:bdiGetPort).
Monitor::Io Monitor::decode(Computer* c) {
	Io io;
	CPU* cpu = c->cpu;
	const int pc = cpu->regPC;
	auto rd = [&](int a) { return memRd(c->mem, a & 0xffff) & 0xff; };
	const int op = rd(pc);
	if (op == 0xd3) { io.out = true; io.port = rd(pc + 1); io.val = cpu->regA; return io; }
	if (op == 0xdb) { io.in = true; io.port = rd(pc + 1); return io; }
	if (op != 0xed) return io;
	const int op2 = rd(pc + 1);
	if ((op2 & 0xc7) == 0x40) { io.in = true; io.port = cpu->regC; return io; }
	if ((op2 & 0xc7) == 0x41) {
		io.out = true;
		io.port = cpu->regC;
		switch ((op2 >> 3) & 7) {
		case 0: io.val = cpu->regB; break;
		case 1: io.val = cpu->regC; break;
		case 2: io.val = cpu->regD; break;
		case 3: io.val = cpu->regE; break;
		case 4: io.val = cpu->regH; break;
		case 5: io.val = cpu->regL; break;
		case 6: io.val = 0; break;		// out (c),0
		default: io.val = cpu->regA; break;
		}
		return io;
	}
	if (op2 == 0xa2 || op2 == 0xaa || op2 == 0xb2 || op2 == 0xba) {
		io.in = true; io.port = cpu->regC; return io;
	}
	if (op2 == 0xa3 || op2 == 0xab || op2 == 0xb3 || op2 == 0xbb) {
		io.out = true; io.port = cpu->regC; io.val = rd(cpu->regHL); return io;
	}
	return io;
}

void Monitor::before(Computer* c, bool intDue) {
	sync(c);
	m_t0 = c->tickCount;
	FDC* f = (c->dif && c->dif->type == DIF_BDI) ? c->dif->fdc : nullptr;
	if (!f || !f->flp) { m_quick = true; m_prev.valid = false; return; }
	// The controller can only change through its ports, which only answer
	// with TR-DOS paged in. Neither, and nothing is running: nothing to see.
	m_quick = !f->plan && !c->flgDOS && !m_isr.open;
	if (m_quick) {
		m_prev.valid = true;
		m_prev.plan = nullptr;
		m_prev.idle = f->idle;
		return;
	}
	// the drive's speed: one byte time for data and rotation alike
	const int drv = driveIndex(f);
	const int base = f->hd ? 16000 : 32000;
	const double rpm = m_tweak[drv].rpmPercent;
	if (rpm != 0.0) {
		f->bytedelay = (int)std::lround(base * 100.0 / (100.0 + rpm));
		m_baseDelay = base;
	} else if (m_baseDelay) {
		f->bytedelay = base;
		m_baseDelay = 0;
	}
	// a step that takes an interrupt runs no instruction: the IN at PC has
	// not happened, and counting it would hide the byte it then misses
	m_io = (c->flgDOS && !intDue) ? decode(c) : Io();
	const bool known = m_prev.valid;
	m_prev = snap(c);
	// after a reset or a load the controller may already be running a command
	// no instruction wrote - the restore a reset starts
	if (!known && m_prev.busy() && !m_openN) startCommand(c, -1, f->com);
	m_pcBefore = cpu_get_pc(c->cpu);
	m_spBefore = cpu_get_sp(c->cpu);
}

Row* Monitor::open() {
	if (!m_openN || m_rows.empty()) return nullptr;
	const long long i = m_openN - m_rows.front().n;
	if (i < 0 || i >= (long long)m_rows.size()) return nullptr;
	return &m_rows[(size_t)i];
}

static Row* rowByN(std::deque<Row>& rows, long long n) {
	if (!n || rows.empty()) return nullptr;
	const long long i = n - rows.front().n;
	if (i < 0 || i >= (long long)rows.size()) return nullptr;
	return &rows[(size_t)i];
}

void Monitor::push(Row&& r) {
	r.n = m_next++;
	m_rows.push_back(std::move(r));
	while (m_rows.size() > kRows) m_rows.pop_front();
}

void Monitor::stop(const std::string& why) {
	if (m_stop.empty()) m_stop = why;
}

static std::string chs(const SectorRec& s) {
	char b[32];
	snprintf(b, sizeof(b), "%d/%d/%d", s.c, s.h, s.s);
	return b;
}

void Monitor::startCommand(Computer* c, int pc, int value) {
	FDC* f = c->dif->fdc;
	Row r;
	r.type = Row::Command;
	r.t = m_T;
	r.frame = c->vid->fcnt;
	r.frameT = c->frmtCount;
	r.pc = pc;
	if (pc >= 0) {
		if (const char* n = trdosNameAt(c, pc)) r.pcName = n;
	}
	r.value = value & 0xff;
	r.kind = kindOf(value);
	r.drive = driveIndex(f);
	r.side = f->side;
	// the registers as the program left them: the step that started the
	// command may already have moved them (a restore loads $FF first)
	const bool before = m_prev.valid && pc >= 0;
	r.trk = before ? m_prev.trk : f->trk;
	r.sec = before ? m_prev.sec : f->sec;
	r.data = f->data;
	r.head = f->flp->trk;
	r.startPos = m_prev.valid && m_prev.flp == f->flp ? m_prev.flpPos : f->flp->pos;
	r.trklen = f->flp->trklen;
	r.fast = (fdcFlag & FDC_FAST) != 0;
	const Kind k = r.kind;
	push(std::move(r));
	m_openN = m_rows.back().n;
	if (m_breaks.command && (m_breaks.kind == Kind::None || m_breaks.kind == k)) {
		char b[96];
		snprintf(b, sizeof(b), "command %s ($%02X)", kindName(k), value & 0xff);
		stop(b);
	}
}

void Monitor::endCommand(Computer* c, const char* how) {
	Row* r = open();
	m_openN = 0;
	if (!r) return;
	FDC* f = c->dif->fdc;
	r->durationT = m_T - r->t;
	r->status = statusAsRead(f) & ~0x01;	// it is over, whatever the stop plan still does
	r->end = how;
	const Kind k = r->kind;
	// a sector whose ID field failed its CRC never reached the data phase
	if ((r->status & 0x08) && (k == Kind::ReadSector || k == Kind::WriteSector) &&
	    r->sectors.empty()) {
		SectorRec s;
		s.c = f->buf[0]; s.h = f->buf[1]; s.s = f->buf[2]; s.n = f->buf[3];
		r->sectors.push_back(s);
	}
	if ((r->status & 0x08) && !r->sectors.empty()) r->sectors.back().crcError = true;

	m_sum.commands++;
	m_sum.byKind[kindName(k)]++;
	m_sum.busyT += r->durationT;
	if (isTypeOne(k)) m_sum.seekT += r->durationT;
	if (m_sum.firstT < 0) m_sum.firstT = r->t;
	m_sum.lastT = m_T;
	if (r->status & 0x08) m_sum.crcErrors++;
	if (!isTypeOne(k) && (r->status & 0x10)) m_sum.notFound++;
	for (const auto& s : r->sectors) {
		if (k != Kind::ReadSector && k != Kind::WriteSector) break;
		m_sum.sectors++;
		if (s.lost) m_sum.sectorsLost++;
		if (s.lost && s.interrupts) m_sum.sectorsHitByIsr++;
		if (!s.lost && !s.crcError && s.dataT >= 0 && s.n >= 0 &&
		    s.delivered >= (128 << (s.n & 3))) {
			m_sum.sectorsOk++;
			if (readsData(k)) m_sum.okBytes += 128 << (s.n & 3);
		}
	}

	if (m_breaks.end) {
		char b[96];
		snprintf(b, sizeof(b), "end of %s: status $%02X (%s)", kindName(k), r->status, how);
		stop(b);
	}
	const int errMask = isTypeOne(k) ? 0x98 : 0xdc;
	if (m_breaks.error && (r->status & errMask)) {
		std::string fl;
		for (const auto& n : statusFlags(r->status & errMask, k)) fl += (fl.empty() ? "" : ",") + n;
		stop(std::string("error in ") + kindName(k) + ": " + fl);
	}
	if (m_breaks.crc && (r->status & 0x08))
		stop(std::string("crc error in ") + kindName(k) +
		     (r->sectors.empty() ? "" : " sector " + chs(r->sectors.back())));
	if (m_breaks.notFound && (r->status & 0x10))
		stop(std::string(isTypeOne(k) ? "seek error in " : "not found in ") + kindName(k));
}

void Monitor::closeIsr(long long now) {
	const long long len = now - m_isr.startT;
	if (len > m_sum.isrMaxT) m_sum.isrMaxT = len;
	if (Row* r = rowByN(m_rows, m_isr.rowN)) {
		if (m_isr.sector >= 0 && m_isr.sector < (int)r->sectors.size()) {
			auto& s = r->sectors[(size_t)m_isr.sector];
			if (len > s.isrMaxT) s.isrMaxT = len;
		}
	}
	m_isr = Isr();
}

void Monitor::after(Computer* c, bool intDue) {
	FDC* f = (c->dif && c->dif->type == DIF_BDI) ? c->dif->fdc : nullptr;
	const long long dt = (long long)(unsigned)((unsigned)c->tickCount - (unsigned)m_t0);
	sync(c);
	if (!f || !f->flp) return;
	if (m_quick && !f->plan) return;
	if (!m_prev.valid || (m_quick && f->plan)) {
		// something outside the instruction stream (a reset, a load) started
		// the controller; the next step has a proper before-picture
		m_prev = snap(c);
		m_quick = false;
		if (f->plan && !f->idle && !m_openN) startCommand(c, -1, f->com);
		return;
	}
	const Snap cur = snap(c);
	const int pc = cpu_get_pc(c->cpu), sp = cpu_get_sp(c->cpu);
	const bool accepted = intDue && !c->cpu->flgIFF1;
	bool startedNow = false;

	// ---- interrupts
	if (m_isr.open && !accepted && pc == m_isr.pc && sp == m_isr.sp) closeIsr(m_T);
	if (accepted && !m_isr.open && m_prev.busy()) {
		m_isr.open = true;
		m_isr.startT = m_T - dt;
		m_isr.pc = m_pcBefore;
		m_isr.sp = m_spBefore;
		m_isr.rowN = m_openN;
		m_sum.interrupts++;
		if (Row* r = open()) {
			if (phaseOf(r->kind, m_prev.step, true) == Phase::Transfer && !r->sectors.empty()) {
				m_isr.sector = (int)r->sectors.size() - 1;
				m_isr.inTransfer = true;
				r->sectors.back().interrupts++;
				m_sum.interruptsInTransfer++;
				if (m_breaks.intInTransfer) {
					char b[96];
					snprintf(b, sizeof(b), "interrupt during the transfer of sector %s at byte %d",
						 chs(r->sectors.back()).c_str(), r->sectors.back().delivered);
					stop(b);
				}
			}
		}
	}

	// ---- the ports
	bool dataAccess = false;
	bool forcedNow = false;
	if (m_io.port >= 0) {
		const int p = m_io.port & 0xff;
		if (m_io.out && (p & 0x9f) == 0x9f) {
			if (m_io.val != m_lastSys) {
				Row r;
				r.type = Row::Sys;
				r.t = m_T; r.frame = c->vid->fcnt; r.frameT = c->frmtCount;
				r.pc = m_pcBefore;
				if (const char* n = trdosNameAt(c, m_pcBefore)) r.pcName = n;
				r.value = m_io.val & 0xff;
				r.drive = m_io.val & 3;
				r.side = (m_io.val & 0x10) ? 0 : 1;
				push(std::move(r));
				m_lastSys = m_io.val;
			}
		} else if (m_io.out && p == 0x1f) {
			const int v = m_io.val & 0xff;
			if ((v & 0xf0) == 0xd0) {
				if (m_openN) endCommand(c, "forced");
				startCommand(c, m_pcBefore, v);
				endCommand(c, "done");
				forcedNow = true;
			} else if (!f->mr) {
				Row r;
				r.type = Row::Ignored; r.why = "reset";
				r.t = m_T; r.frame = c->vid->fcnt; r.frameT = c->frmtCount;
				r.pc = m_pcBefore; r.value = v; r.kind = kindOf(v);
				if (const char* n = trdosNameAt(c, m_pcBefore)) r.pcName = n;
				push(std::move(r));
				m_sum.ignored++;
			} else if (!m_prev.idle) {
				Row r;
				r.type = Row::Ignored; r.why = "busy";
				r.t = m_T; r.frame = c->vid->fcnt; r.frameT = c->frmtCount;
				r.pc = m_pcBefore; r.value = v; r.kind = kindOf(v);
				if (const char* n = trdosNameAt(c, m_pcBefore)) r.pcName = n;
				push(std::move(r));
				m_sum.ignored++;
			} else {
				if (m_openN) endCommand(c, "done");
				startCommand(c, m_pcBefore, v);
				startedNow = true;
			}
		} else if (p == 0x7f) {
			dataAccess = true;
			m_sum.taken++;
		} else if (m_io.in && (p == 0x1f || (p & 0x9f) == 0x9f)) {
			m_sum.polls++;
		}
	}
	// a command nobody wrote: the restore a reset runs
	if (!m_openN && !forcedNow && cur.busy() && !m_prev.busy()) {
		startCommand(c, -1, f->com);
		startedNow = true;
	}

	// ---- the disk turning under the head
	int moved = 0;
	bool wrapped = false;
	if (cur.flp == m_prev.flp && cur.flp && m_prev.plan) {
		moved = cur.flpPos - m_prev.flpPos;
		if (moved < 0) {
			moved += cur.flp->trklen;
			wrapped = true;
			if (m_breaks.index) stop("index pulse");
		}
	}
	// the drive setup's spin-up: a motor that just came on is not at speed
	{
		const int drv = driveIndex(f);
		if (m_tweak[drv].spinupMs > 0 && cur.flp == m_prev.flp && !m_prev.motor && cur.motor) {
			const long long ns = (long long)m_tweak[drv].spinupMs * 1000000LL;
			f->wait += (int)ns;
			if (Row* r = open()) r->spinupNs += ns;
		}
	}
	// an ID field starting under the head
	if (m_breaks.id && moved > 0 && moved < 64) {
		const Floppy* flp = cur.flp;
		const int tr = (flp->trk << 1) | ((flp->doubleSide && f->side) ? 1 : 0);
		const int len = flp->trklen;
		for (int i = 1; i <= moved; i++) {
			const int p = (m_prev.flpPos + i) % len;
			const int q = (p + len - 1) % len;
			if ((flp->data[tr].field[p] & 0x0f) != 1 || (flp->data[tr].field[q] & 0x0f) == 1)
				continue;
			const unsigned char* b = flp->data[tr].byte;
			if (b[p] == m_breaks.idC && b[(p + 1) % len] == m_breaks.idH &&
			    b[(p + 2) % len] == m_breaks.idS) {
				char t[64];
				snprintf(t, sizeof(t), "id %d/%d/%d under the head", m_breaks.idC,
					 m_breaks.idH, m_breaks.idS);
				stop(t);
			}
		}
	}

	// ---- progress of the running command
	Row* r = open();
	if (r && !startedNow) {
		const Kind k = r->kind;
		const Phase pp = phaseOf(k, m_prev.step, m_prev.busy());
		const Phase cp = phaseOf(k, cur.step, cur.busy());
		if (wrapped) {
			r->revolutions++;
			m_sum.revolutions++;
		}
		// an ID matched: the search moved on to the data mark
		if ((k == Kind::ReadSector || k == Kind::WriteSector) && pp == Phase::FindId &&
		    cp != Phase::FindId && cp != Phase::Idle && cp != Phase::Prepare) {
			SectorRec s;
			s.c = f->buf[0]; s.h = f->buf[1]; s.s = f->buf[2]; s.n = f->buf[3];
			s.foundT = m_T - r->t;
			r->sectors.push_back(s);
		}
		// read address: the ID that is passing the head now
		if (k == Kind::ReadAddress && pp == Phase::FindId && cp == Phase::Transfer) {
			const Floppy* flp = cur.flp;
			const TrackLayout lay = trackLayout(flp, flp->trk, (flp->doubleSide && f->side) ? 1 : 0);
			int idx = -1;
			fieldAt(flp, flp->trk, f->side, cur.flpPos, lay, &idx);
			SectorRec s;
			if (idx >= 0) {
				const auto& l = lay.sectors[(size_t)idx];
				s.c = l.c; s.h = l.h; s.s = l.s; s.n = l.n;
			}
			s.foundT = m_T - r->t;
			s.dataT = s.foundT;
			r->sectors.push_back(s);
		}
		if (cp == Phase::Transfer && pp != Phase::Transfer && k != Kind::ReadAddress) {
			if (r->sectors.empty()) r->sectors.push_back(SectorRec());	// track commands
			auto& s = r->sectors.back();
			s.dataT = m_T - r->t;
			if (m_isr.open && m_isr.sector < 0) {
				m_isr.sector = (int)r->sectors.size() - 1;
				m_isr.inTransfer = true;
				s.interrupts++;
				m_sum.interruptsInTransfer++;
			}
		}
		if (pp == Phase::Transfer && moved > 0 && !r->sectors.empty()) {
			auto& s = r->sectors.back();
			const int before = s.delivered;
			s.delivered += moved;
			m_sum.delivered += moved;
			// a new byte arrived while the last one was still waiting: that one
			// is gone (wd1793.c:vgGetByte/vgSendByte set the lost-data bit)
			int lostNow = (m_prev.drq && !dataAccess) ? 1 : 0;
			lostNow += moved - 1;
			if (lostNow > 0) {
				if (s.firstLost < 0) s.firstLost = before > 0 ? before - 1 : 0;
				s.lost += lostNow;
				m_sum.lost += lostNow;
				if (m_breaks.lost) {
					char b[96];
					snprintf(b, sizeof(b), "lost data: sector %s byte %d", chs(s).c_str(),
						 s.firstLost);
					stop(b);
				}
			}
		}
		if (pp == Phase::Transfer && cp != Phase::Transfer && !r->sectors.empty()) {
			auto& s = r->sectors.back();
			s.endT = m_T - r->t;
			if (s.dataT >= 0) m_sum.transferT += s.endT - s.dataT;
		}
		if (pp == Phase::FindId && cp != Phase::FindId && !r->sectors.empty() &&
		    r->sectors.back().foundT == m_T - r->t) {
			// time spent waiting for the sector to come round, counted from the
			// command start or the previous sector's end
			long long from = 0;
			if (r->sectors.size() >= 2) {
				const auto& p = r->sectors[r->sectors.size() - 2];
				from = p.endT >= 0 ? p.endT : p.foundT;
			}
			const long long w = r->sectors.back().foundT - from;
			if (w > 0) m_sum.findT += w;
		}
	}

	// ---- the end
	if (m_openN && !cur.busy() && !startedNow) {
		endCommand(c, (!cur.plan && !cur.idle) ? "reset" : "done");
	} else if (m_openN && startedNow && !cur.busy()) {
		endCommand(c, "done");		// over within the step that wrote it (no disk)
	}
	m_prev = cur;
}

void Monitor::discontinuity(Computer* c, const char* why) {
	if (c) sync(c);
	if (m_openN && c && c->dif && c->dif->fdc) endCommand(c, why);
	m_openN = 0;
	m_isr = Isr();
	m_prev.valid = false;
	m_stop.clear();
	// a load put the core's T counter wherever the saved machine had it
	if (c) m_tick = c->tickCount;
}

void Monitor::clear(const Computer* c) {
	sync(c);
	m_rows.clear();
	m_openN = 0;
	m_sum = Summary();
	m_sum.sinceT = m_T;
	m_isr = Isr();
}

Monitor::Saved Monitor::save() const {
	Saved s;
	s.rows = m_rows;
	s.next = m_next;
	s.openN = m_openN;
	s.sum = m_sum;
	s.T = m_T;
	s.lastSys = m_lastSys;
	s.isr = m_isr;
	return s;
}

void Monitor::restore(const Saved& s) {
	m_rows = s.rows;
	m_next = s.next;
	m_openN = s.openN;
	m_sum = s.sum;
	m_T = s.T;
	m_lastSys = s.lastSys;
	m_isr = s.isr;
	m_prev.valid = false;
	m_stop.clear();
	m_synced = false;		// the next sync starts from wherever the core is
}

std::vector<std::string> Breaks::names() const {
	std::vector<std::string> n;
	if (command) n.push_back(kind == Kind::None ? "command" : std::string("command:") + kindName(kind));
	if (end) n.push_back("end");
	if (error) n.push_back("error");
	if (lost) n.push_back("lost_data");
	if (crc) n.push_back("crc_error");
	if (notFound) n.push_back("not_found");
	if (index) n.push_back("index");
	if (intInTransfer) n.push_back("interrupt_in_transfer");
	if (id) {
		char b[48];
		snprintf(b, sizeof(b), "id:%d/%d/%d", idC, idH, idS);
		n.push_back(b);
	}
	return n;
}

// ---------------------------------------------------------------- faults

const char* Faults::kinds() { return "crc_data | crc_id | no_id | no_data | deleted"; }

bool Faults::add(Floppy* flp, int drive, int cyl, int side, int sector, const std::string& kind,
		 std::string& err) {
	if (!flp || !flp->insert) { err = "no disk in drive " + std::to_string(drive); return false; }
	if (cyl < 0 || cyl > 127 || side < 0 || side > 1) { err = "no such cylinder/side"; return false; }
	const int tr = trackIndex(flp, cyl, side);
	const TrackLayout lay = trackLayout(flp, cyl, side);
	const SectorLayout* s = nullptr;
	for (const auto& l : lay.sectors)
		if (l.s == sector) { s = &l; break; }
	if (!s) {
		err = "sector " + std::to_string(sector) + " is not on cylinder " + std::to_string(cyl) +
		      " side " + std::to_string(side);
		return false;
	}
	const bool needData = kind == "crc_data" || kind == "no_data" || kind == "deleted";
	if (needData && s->dataAt < 0) { err = "that sector has no data field"; return false; }
	if (kind != "crc_data" && kind != "crc_id" && kind != "no_id" && kind != "no_data" &&
	    kind != "deleted") {
		err = "unknown fault '" + kind + "' (" + kinds() + ")";
		return false;
	}
	const int key = (drive << 16) | tr;
	const int len = flp->trklen;
	if (!m_orig.count(key)) {
		Saved sv;
		sv.bytes.assign(flp->data[tr].byte, flp->data[tr].byte + len);
		sv.fields.assign(flp->data[tr].field, flp->data[tr].field + len);
		memcpy(sv.map, flp->data[tr].map, sizeof(sv.map));
		m_orig[key] = std::move(sv);
	}
	unsigned char* b = flp->data[tr].byte;
	auto at = [&](int i) -> unsigned char& { return b[((i % len) + len) % len]; };
	if (kind == "crc_data") {
		at(s->dataAt + s->size) ^= 0xff;
	} else if (kind == "crc_id") {
		at(s->idAt + 4) ^= 0xff;
	} else if (kind == "no_id") {
		at(s->idAt - 1) = 0x00;
		flpFillFields(flp, tr, 0);
	} else if (kind == "no_data") {
		at(s->dataAt - 1) = 0x00;
		flpFillFields(flp, tr, 0);
	} else {	// deleted: an $F8 mark with a CRC that is right for it
		at(s->dataAt - 1) = 0xf8;
		std::vector<unsigned char> buf{0xf8};
		for (int k = 0; k < s->size; k++) buf.push_back(at(s->dataAt + k));
		const unsigned short crc = fieldCrc(buf.data(), (int)buf.size());
		at(s->dataAt + s->size) = crc >> 8;
		at(s->dataAt + s->size + 1) = crc & 0xff;
		flpFillFields(flp, tr, 0);
	}
	m_list.push_back(Fault{drive, cyl, side, sector, kind});
	return true;
}

int Faults::clear(Floppy* const* flps, int drive) {
	int n = 0;
	for (auto it = m_orig.begin(); it != m_orig.end();) {
		const int drv = it->first >> 16, tr = it->first & 0xffff;
		if (drive >= 0 && drv != drive) { ++it; continue; }
		Floppy* flp = flps[drv & 3];
		const size_t len = it->second.bytes.size();
		if (flp && (int)len <= TRKLEN_HD) {
			memcpy(flp->data[tr].byte, it->second.bytes.data(), len);
			memcpy(flp->data[tr].field, it->second.fields.data(), len);
			memcpy(flp->data[tr].map, it->second.map, sizeof(it->second.map));
		}
		it = m_orig.erase(it);
	}
	for (auto it = m_list.begin(); it != m_list.end();) {
		if (drive >= 0 && it->drive != drive) { ++it; continue; }
		it = m_list.erase(it);
		n++;
	}
	return n;
}

void Faults::forget(int drive) {
	for (auto it = m_orig.begin(); it != m_orig.end();)
		it = (drive < 0 || (it->first >> 16) == drive) ? m_orig.erase(it) : std::next(it);
	for (auto it = m_list.begin(); it != m_list.end();)
		it = (drive < 0 || it->drive == drive) ? m_list.erase(it) : std::next(it);
}

std::vector<Fault> Faults::list(int drive) const {
	std::vector<Fault> out;
	for (const auto& f : m_list)
		if (drive < 0 || f.drive == drive) out.push_back(f);
	return out;
}

std::string Faults::kindAt(int drive, int cyl, int side, int sector) const {
	std::string k;
	for (const auto& f : m_list)
		if (f.drive == drive && f.cyl == cyl && f.side == side && f.sector == sector)
			k += (k.empty() ? "" : ",") + f.kind;
	return k;
}

} // namespace fdc
} // namespace xsp
