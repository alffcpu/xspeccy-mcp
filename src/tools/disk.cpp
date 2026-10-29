// The Beta Disk interface for a debugger: the WD1793 now and in the past,
// stops on its events, the disk's physical layout, faults put on it, the same
// load at every angle of the disk, and whole-machine checkpoints that keep a
// controller mid-command. The monitor that feeds most of this is xsp_fdc.cpp.
#include "tools/common.h"

#include <cstdio>

namespace xsp {
namespace tools {

namespace {

std::string hex8(int v) {
	char b[8];
	snprintf(b, sizeof(b), "$%02X", v & 0xff);
	return b;
}

// The machine, if it has the controller these tools speak.
Computer* bdi() {
	Computer* c = mach().comp();
	if (!c->dif || c->dif->type != DIF_BDI)
		throw std::runtime_error(std::string("this machine has no Beta Disk interface (disk: ") +
					 diskInterfaceName(c->dif ? c->dif->type : DIF_NONE) +
					 "); these tools speak the WD1793 of the Beta Disk");
	return c;
}

int argDrive(const json& a) {
	const int drv = argNumOr(a, "drive", 0);
	if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
	return drv;
}

Floppy* diskIn(Computer* c, int drv) {
	Floppy* flp = c->dif->flp[drv];
	if (!flp->insert) throw std::runtime_error("no disk in drive " + std::to_string(drv));
	return flp;
}

// T-states of one disk byte at the controller's present speed
double byteT(Computer* c) {
	const Timing tm = timing(c);
	return tm.nsPerT > 0 ? c->dif->fdc->bytedelay / tm.nsPerT : 0.0;
}

long long roundT(double v) { return (long long)std::llround(v); }

json idJson(int c, int h, int s, int n) { return json::array({c, h, s, n}); }

json sectorJson(const fdc::SectorRec& s) {
	json j = json::object();
	if (s.c >= 0) j["id"] = idJson(s.c, s.h, s.s, s.n);
	if (s.foundT >= 0) j["found_t"] = s.foundT;
	if (s.dataT >= 0) j["data_t"] = s.dataT;
	j["bytes"] = s.delivered;
	if (s.lost) {
		j["lost"] = s.lost;
		j["first_lost"] = s.firstLost;
	}
	if (s.interrupts) {
		j["interrupts"] = s.interrupts;
		if (s.isrMaxT) j["isr_max_t"] = s.isrMaxT;
	}
	if (s.crcError) j["crc_error"] = true;
	return j;
}

json rowJson(const fdc::Row& r) {
	json j = {{"n", r.n}, {"t", r.t}, {"frame", r.frame}, {"frame_t", r.frameT}};
	if (r.pc >= 0) j["pc_hex"] = hex16(r.pc);
	if (!r.pcName.empty()) j["pc_name"] = r.pcName;
	if (r.type == fdc::Row::Sys) {
		j["sys_hex"] = hex8(r.value);
		j["drive"] = r.drive;
		j["side"] = r.side;
		if (!(r.value & 0x04)) j["reset"] = true;
		return j;
	}
	if (r.type == fdc::Row::Ignored) {
		j["ignored"] = fdc::kindName(r.kind);
		j["value_hex"] = hex8(r.value);
		j["why"] = r.why;
		return j;
	}
	j["cmd"] = fdc::kindName(r.kind);
	j["value_hex"] = hex8(r.value);
	j["drive"] = r.drive;
	j["side"] = r.side;
	j["track"] = r.trk;
	j["sector"] = r.sec;
	j["head"] = r.head;
	j["disk_pos"] = r.startPos;
	if (r.durationT >= 0) {
		j["duration_t"] = r.durationT;
		j["status_hex"] = hex8(r.status);
		j["status"] = fdc::statusFlags(r.status, r.kind);
		j["end"] = r.end;
	} else {
		j["end"] = "running";
	}
	if (r.revolutions) j["revolutions"] = r.revolutions;
	if (r.spinupNs) j["spinup_ms"] = r.spinupNs / 1000000;
	if (r.fast) j["turbo"] = true;
	if (!r.sectors.empty()) {
		json ss = json::array();
		for (const auto& s : r.sectors) ss.push_back(sectorJson(s));
		j["sectors"] = ss;
	}
	return j;
}

// Data per revolution of the track under the head over the time of one
// revolution: what a perfect loader reading this side could get.
double ceilingBytesPerS(Computer* c) {
	const FDC* f = c->dif->fdc;
	const Floppy* flp = f->flp;
	if (!flp || !flp->insert || flp->trklen <= 0) return 0.0;
	const fdc::TrackLayout lay = fdc::trackLayout(flp, flp->trk, (flp->doubleSide && f->side) ? 1 : 0);
	long long bytes = 0;
	for (const auto& s : lay.sectors)
		if (s.dataAt >= 0) bytes += s.size;
	const double revS = (double)flp->trklen * f->bytedelay / 1e9;
	return revS > 0 ? bytes / revS : 0.0;
}

json summaryJson(Computer* c, const fdc::Summary& s) {
	const Timing tm = timing(c);
	json byKind = json::object();
	for (const auto& kv : s.byKind) byKind[kv.first] = kv.second;
	json j = {
		{"since_t", s.sinceT},
		{"commands", s.commands},
		{"by_kind", byKind},
		{"sectors", s.sectors},
		{"sectors_ok", s.sectorsOk},
		{"sectors_lost_data", s.sectorsLost},
		{"crc_errors", s.crcErrors},
		{"not_found", s.notFound},
		{"revolutions", s.revolutions},
		{"t", {{"busy", s.busyT}, {"seek", s.seekT}, {"find_id", s.findT}, {"transfer", s.transferT}}},
		{"bytes", {{"delivered", s.delivered}, {"lost", s.lost}, {"data_port", s.taken},
			   {"status_polls", s.polls}}},
		{"interrupts", {{"during_commands", s.interrupts},
				{"during_transfer", s.interruptsInTransfer},
				{"sectors_hit", s.sectorsHitByIsr},
				{"longest_handler_t", s.isrMaxT},
				{"byte_window_t", roundT(byteT(c))}}},
	};
	if (s.ignored) j["ignored"] = s.ignored;
	if (s.firstT >= 0 && s.lastT > s.firstT && tm.nsPerT > 0) {
		const double secs = (s.lastT - s.firstT) * tm.nsPerT / 1e9;
		j["throughput"] = {{"span_t", s.lastT - s.firstT},
				   {"bytes_ok", s.okBytes},
				   {"bytes_per_s", (long long)std::llround(s.okBytes / secs)},
				   {"ceiling_bytes_per_s", (long long)std::llround(ceilingBytesPerS(c))}};
	}
	return j;
}

// TR-DOS's own variables, read where the CPU sees them now.
json trdosVarsJson() {
	json j = json::object();
	for (const auto& v : fdc::trdosVars()) {
		if (v.size == 1) {
			j[v.name] = mach().readByte(v.address);
		} else if (v.size == 2) {
			j[v.name] = mach().readByte(v.address) | (mach().readByte(v.address + 1) << 8);
		} else if (v.size == 16) {
			std::string name;
			for (int i = 0; i < 8; i++) {
				const int ch = mach().readByte(v.address + i);
				name += (ch >= 32 && ch < 127) ? (char)ch : '?';
			}
			const int type = mach().readByte(v.address + 8);
			auto w = [&](int o) { return mach().readByte(v.address + o) | (mach().readByte(v.address + o + 1) << 8); };
			j[v.name] = {{"name", name},
				     {"type", std::string(1, (type >= 32 && type < 127) ? (char)type : '?')},
				     {"start", w(9)}, {"length", w(11)},
				     {"sectors", mach().readByte(v.address + 13)},
				     {"sector", mach().readByte(v.address + 14)},
				     {"track", mach().readByte(v.address + 15)}};
		} else {
			json arr = json::array();
			for (int i = 0; i < v.size; i++) arr.push_back(mach().readByte(v.address + i));
			j[v.name] = arr;
		}
	}
	return j;
}

// TR-DOS's order: sectors 1..16 of a side, then the other side, then the next
// cylinder (logical track = cylinder * 2 + side).
struct Place {
	int cyl, side, sec;
};

void nextPlace(const Floppy* flp, Place& p) {
	const fdc::TrackLayout lay = fdc::trackLayout(flp, p.cyl, p.side);
	for (const auto& s : lay.sectors)
		if (s.s == p.sec + 1) { p.sec++; return; }
	p.sec = 1;
	if (flp->doubleSide && p.side == 0) {
		p.side = 1;
	} else {
		p.side = 0;
		p.cyl++;
	}
}

// the data of one sector as the image holds it; empty when it is not there
std::vector<unsigned char> sectorData(const Floppy* flp, const Place& p, fdc::SectorLayout* out = nullptr) {
	const fdc::TrackLayout lay = fdc::trackLayout(flp, p.cyl, p.side);
	for (const auto& s : lay.sectors) {
		if (s.s != p.sec || s.dataAt < 0) continue;
		if (out) *out = s;
		std::vector<unsigned char> d((size_t)s.size);
		const unsigned char* b = flp->data[(p.cyl << 1) | p.side].byte;
		for (int i = 0; i < s.size; i++) d[(size_t)i] = b[(s.dataAt + i) % lay.trklen];
		return d;
	}
	return {};
}

// Where to start comparing: a cylinder/side/sector, or a file named in the
// TR-DOS catalogue - whose length is the default length.
void verifyStart(const json& a, const Floppy* flp, Place& p, int& length) {
	const std::string file = argStr(a, "file");
	if (!file.empty()) {
		TRFile cat[128];
		const int n = diskGetTRCatalog(const_cast<Floppy*>(flp), cat);
		std::string want = file, ext;
		const size_t dot = file.rfind('.');
		if (dot != std::string::npos && dot + 2 == file.size()) {
			want = file.substr(0, dot);
			ext = file.substr(dot + 1);
		}
		for (int i = 0; i < n; i++) {
			if (cat[i].name[0] < 2) continue;
			std::string nm((const char*)cat[i].name, 8);
			while (!nm.empty() && nm.back() == ' ') nm.pop_back();
			if (nm != want || (!ext.empty() && ext[0] != (char)cat[i].ext)) continue;
			p.cyl = cat[i].trk >> 1;
			p.side = cat[i].trk & 1;
			if (!flp->doubleSide) { p.cyl = cat[i].trk; p.side = 0; }
			p.sec = cat[i].sec + 1;
			if (length <= 0) {
				length = (cat[i].hlen << 8) | cat[i].llen;
				if (length <= 0) length = cat[i].slen * 256;
			}
			return;
		}
		throw std::runtime_error("no file '" + file + "' in the catalogue of the disk");
	}
	p.cyl = argNumRange(a, "cylinder", -1, 0, 127);
	p.side = argNumRange(a, "side", 0, 0, 1);
	p.sec = argNumRange(a, "sector", 1, 1, 255);
	if (p.cyl < 0) throw std::runtime_error("cylinder (with side and sector) or file required");
}

// Memory against the disk, sector after sector. The answer an agent wants
// after a load: did every byte arrive, and if not, the first one that did not.
json verifyMemory(const Floppy* flp, int address, int length, Place p) {
	if (length <= 0) throw std::runtime_error("length required");
	if (address < 0 || address + length > 0x10000)
		throw std::runtime_error("address + length must stay inside the 64K address space");
	long long mismatches = 0;
	json first;
	int done = 0;
	while (done < length) {
		fdc::SectorLayout sl;
		const std::vector<unsigned char> d = sectorData(flp, p, &sl);
		if (d.empty())
			throw std::runtime_error("sector " + std::to_string(p.cyl) + "/" + std::to_string(p.side) +
						 "/" + std::to_string(p.sec) + " is not on the disk (after " +
						 std::to_string(done) + " bytes)");
		for (size_t i = 0; i < d.size() && done < length; i++, done++) {
			const int adr = address + done;
			const int m = mach().readByte(adr);
			if (m == d[i]) continue;
			if (!mismatches)
				first = {{"address_hex", hex16(adr)},
					 {"sector", json::array({p.cyl, p.side, p.sec})},
					 {"offset", (int)i},
					 {"memory_hex", hex8(m)}, {"disk_hex", hex8(d[i])}};
			mismatches++;
		}
		nextPlace(flp, p);
	}
	json j = {{"ok", mismatches == 0}, {"bytes", length}};
	if (mismatches) {
		j["mismatches"] = mismatches;
		j["first"] = first;
	}
	return j;
}

std::string hexBytes(const std::vector<unsigned char>& d) {
	static const char* x = "0123456789ABCDEF";
	std::string s;
	s.reserve(d.size() * 2);
	for (unsigned char b : d) { s += x[b >> 4]; s += x[b & 15]; }
	return s;
}

const char* markName(int mark) {
	return mark == 0xfb ? "data" : mark == 0xf8 ? "deleted" : mark ? "other" : "none";
}

} // namespace

void registerDiskTools() {
	tool("fdc_state",
	     "The Beta Disk's WD1793 right now, read without side effects: registers, status "
	     "decoded for the command class, INTRQ/DRQ, the running command and its phase, the "
	     "selected drive (head cylinder against the track register, motor, protect), and the "
	     "disk under the head - position, what field and sector it is in, and in how many "
	     "T-states each sector's ID comes round. trdos: true adds TR-DOS's system variables.",
	     json{{"properties", {{"trdos", {{"type", "boolean"}}}}}},
	     [](const json& a) {
		     Computer* c = bdi();
		     FDC* f = c->dif->fdc;
		     Floppy* flp = f->flp;
		     const int drv = [&] { for (int i = 0; i < 4; i++) if (f->flop[i] == flp) return i; return 0; }();
		     const fdc::Kind k = fdc::kindOf(f->com);
		     const bool busy = f->plan && !f->idle;
		     const fdc::Phase ph = fdc::phaseOf(k, f->pos, busy);
		     const int st = fdc::statusAsRead(f);
		     const bool typeOneStatus = f->fmode == 0;
		     json cmd = {{"value_hex", hex8(f->com)}, {"kind", fdc::kindName(k)},
				 {"busy", busy}, {"phase", fdc::phaseName(ph)}};
		     if (ph == fdc::Phase::Transfer) cmd["bytes_left"] = f->cnt;
		     if (f->state & 0x04) cmd["lost_data"] = true;
		     json reg = {{"track", f->trk}, {"sector", f->sec}, {"data_hex", hex8(f->data)},
				 {"status_hex", hex8(st)},
				 {"status", fdc::statusFlags(st, typeOneStatus ? fdc::Kind::Restore : fdc::Kind::ReadSector)}};
		     const auto& tw = mach().fdc().tweak(drv);
		     json drive = {{"index", drv}, {"inserted", flp->insert ? true : false},
				   {"write_protect", flp->protect ? true : false},
				   {"motor", flp->motor ? true : false},
				   {"head_cylinder", flp->trk},
				   {"track_register_matches", f->trk == flp->trk},
				   {"side", f->side ? 1 : 0},
				   {"double_sided", flp->doubleSide ? true : false},
				   {"tracks_80", flp->trk80 ? true : false},
				   {"image", flp->path ? flp->path : ""}};
		     if (tw.rpmPercent != 0.0) drive["rpm_percent"] = tw.rpmPercent;
		     if (tw.spinupMs) drive["spinup_ms"] = tw.spinupMs;
		     const size_t nf = mach().faults().list(drv).size();
		     if (nf) drive["faults"] = nf;
		     json j = {{"controller", "wd1793"}, {"trdos_paged", c->flgDOS ? true : false},
			       {"command", cmd}, {"registers", reg},
			       {"intrq", f->irq ? true : false}, {"drq", f->drq ? true : false},
			       {"system", {{"drive", drv}, {"side", f->side ? 1 : 0},
					   {"reset", f->mr ? false : true}, {"mfm", f->mfm ? false : true}}},
			       {"drive", drive}};
		     if (fdcFlag & FDC_FAST) j["turbo"] = true;
		     if (flp->insert && flp->trklen > 0) {
			     const int side = (flp->doubleSide && f->side) ? 1 : 0;
			     const fdc::TrackLayout lay = fdc::trackLayout(flp, flp->trk, side);
			     const double bt = byteT(c);
			     int idx = -1;
			     const std::string field = fdc::fieldAt(flp, flp->trk, side, flp->pos, lay, &idx);
			     json under = {{"field", field}};
			     if (idx >= 0) {
				     const auto& s = lay.sectors[(size_t)idx];
				     under["sector"] = idJson(s.c, s.h, s.s, s.n);
				     if (field == "data")
					     under["offset"] = ((flp->pos - s.dataAt) % lay.trklen + lay.trklen) % lay.trklen;
			     }
			     std::vector<std::pair<int, int>> next;	// bytes ahead, sector
			     for (const auto& s : lay.sectors)
				     next.push_back({((s.idAt - flp->pos) % lay.trklen + lay.trklen) % lay.trklen, s.s});
			     std::sort(next.begin(), next.end());
			     json nx = json::array();
			     for (const auto& p : next) nx.push_back({{"sector", p.second}, {"in_t", roundT(p.first * bt)}});
			     j["disk"] = {{"spinning", (f->plan && flp->motor) ? true : false},
					  {"position", flp->pos}, {"trklen", flp->trklen},
					  {"angle_deg", std::round(flp->pos * 3600.0 / flp->trklen) / 10.0},
					  {"byte_t", std::round(bt * 10) / 10},
					  {"revolution_t", roundT(bt * flp->trklen)},
					  {"under_head", under}, {"next_ids", nx}};
		     }
		     if (argBool(a, "trdos", false)) j["trdos"] = trdosVarsJson();
		     return j;
	     });

	tool("fdc_log",
	     "Journal of the WD1793, always on: a row per command (frame, T, the PC that wrote "
	     "it with its TR-DOS name, registers, head, disk angle, duration, status, how it "
	     "ended, revolutions) with a record per sector (ID, when it was found, bytes, lost "
	     "bytes and the first, interrupts and their longest handler, CRC); rows for #FF writes "
	     "and for commands the controller ignored. action: read (default; count, since = the "
	     "last row number you saw), summary (totals since clear: sectors read and broken, "
	     "time finding IDs / transferring / seeking, revolutions, throughput against the "
	     "track's ceiling, interrupts against the byte window), clear.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "read | summary | clear"}}},
		     {"count", {{"type", "integer"}, {"description", "rows, newest last; default 20"}}},
		     {"since", {{"type", "integer"}, {"description", "only rows after this n"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = bdi();
		     auto& mon = mach().fdc();
		     const std::string action = argStr(a, "action", "read");
		     if (action == "clear") {
			     mon.clear(c);
			     return json{{"cleared", true}, {"since_t", mon.now(c)}};
		     }
		     if (action == "summary") return summaryJson(c, mon.summary());
		     if (action != "read") throw std::runtime_error("unknown fdc_log action '" + action + "'");
		     const int count = argNumRange(a, "count", 20, 1, (int)fdc::Monitor::kRows);
		     const long long since = argNumOr(a, "since", 0);
		     const auto& rows = mon.rows();
		     std::vector<const fdc::Row*> pick;
		     for (auto it = rows.rbegin(); it != rows.rend() && (int)pick.size() < count; ++it) {
			     if (it->n <= since) break;
			     pick.push_back(&*it);
		     }
		     json out = json::array();
		     for (auto it = pick.rbegin(); it != pick.rend(); ++it) out.push_back(rowJson(**it));
		     json j = {{"rows", out}, {"last", rows.empty() ? 0 : rows.back().n}, {"now_t", mon.now(c)}};
		     if (!rows.empty() && !pick.empty() && pick.back()->n > rows.front().n)
			     j["earlier"] = (long long)(pick.back()->n - std::max(since, rows.front().n - 1) - 1);
		     return j;
	     });

	tool("fdc_break",
	     "Stop run/run_frames/run_to_beam/step_over/step_out on a WD1793 event, after the "
	     "instruction it happened on; the reply has reason \"fdc\" and fdc_event. on: any of "
	     "command, end, error, lost_data, crc_error, not_found, index, interrupt_in_transfer. "
	     "command: a class name (read_sector...) or value to stop only on that command. id: "
	     "{cylinder, side, sector} stops when that ID passes the head. clear: true drops them "
	     "all (clear_breakpoints does too). Armed until cleared.",
	     json{{"properties", {
		     {"on", {{"type", "array"}, {"items", {{"type", "string"}}}}},
		     {"command", {{"type", "string"}}},
		     {"id", {{"type", "object"}}},
		     {"clear", {{"type", "boolean"}}}
	     }}},
	     [](const json& a) {
		     bdi();
		     fdc::Breaks& b = mach().fdc().breaks();
		     if (argBool(a, "clear", false)) b = fdc::Breaks();
		     if (a.contains("on")) {
			     if (!a["on"].is_array()) throw std::runtime_error("on must be an array of event names");
			     for (const auto& e : a["on"]) {
				     const std::string n = e.is_string() ? e.get<std::string>() : "";
				     if (n == "command") b.command = true;
				     else if (n == "end") b.end = true;
				     else if (n == "error") b.error = true;
				     else if (n == "lost_data") b.lost = true;
				     else if (n == "crc_error") b.crc = true;
				     else if (n == "not_found") b.notFound = true;
				     else if (n == "index") b.index = true;
				     else if (n == "interrupt_in_transfer") b.intInTransfer = true;
				     else throw std::runtime_error("unknown fdc event '" + n + "' (command, end, error, "
								   "lost_data, crc_error, not_found, index, "
								   "interrupt_in_transfer)");
			     }
		     }
		     const std::string cmd = argStr(a, "command");
		     if (!cmd.empty()) {
			     fdc::Kind k = fdc::kindFromName(cmd);
			     int v = 0;
			     if (k == fdc::Kind::None && parseNumber(cmd, v)) k = fdc::kindOf(v);
			     if (k == fdc::Kind::None) throw std::runtime_error("no WD1793 command '" + cmd + "'");
			     b.command = true;
			     b.kind = k;
		     }
		     if (a.contains("id")) {
			     const json& id = a["id"];
			     b.idC = argNumRange(id, "cylinder", -1, 0, 255);
			     b.idH = argNumRange(id, "side", 0, 0, 255);
			     b.idS = argNumRange(id, "sector", -1, 0, 255);
			     if (b.idC < 0 || b.idS < 0) throw std::runtime_error("id needs cylinder and sector");
			     b.id = true;
		     }
		     return json{{"armed", b.names()}};
	     });

	tool("fdc_sweep",
	     "Run the same stretch from the current state once per disk angle and compare. An "
	     "emulator turns the disk the same way every time, so a loader always meets its sectors "
	     "at the same moments against the interrupts; real hardware starts at a random angle. "
	     "For each of `angles` evenly spaced angles: restore the state, turn the disk, run to "
	     "stop_pc (or max_frames), collect the controller summary and, with verify, compare "
	     "the loaded memory with the disk. The machine is put back as it was; profile, trace, "
	     "coverage, beam_log, the fdc journal and fdc breaks are left untouched.",
	     json{{"properties", {
		     {"stop_pc", {{"type", "string"}, {"description", "where a run is done; label names work"}}},
		     {"angles", {{"type", "integer"}, {"description", "2..64, default 8"}}},
		     {"max_frames", {{"type", "integer"}, {"description", "per run, default 500"}}},
		     {"verify", {{"type", "object"}, {"description",
			     "{address, length, file | cylinder, side, sector, drive}: compare after each run"}}}
	     }}, {"required", json::array({"stop_pc"})}},
	     [](const json& a) {
		     Computer* c = bdi();
		     int stopPc = -1;
		     argAddr(a, "stop_pc", stopPc);
		     if (stopPc < 0) throw std::runtime_error("stop_pc required");
		     const int n = argNumRange(a, "angles", 8, 2, 64);
		     const int maxFrames = argNumRange(a, "max_frames", 500, 1, kMaxFrames);
		     const bool doVerify = a.contains("verify") && a["verify"].is_object();
		     json vspec = doVerify ? a["verify"] : json::object();
		     int vDrive = 0, vAddr = 0, vLen = -1;
		     Place vPlace{0, 0, 1};
		     if (doVerify) {
			     vDrive = argDrive(vspec);
			     vAddr = argAddrRequired(vspec, "address");
			     vLen = argNumOr(vspec, "length", -1);
			     verifyStart(vspec, diskIn(c, vDrive), vPlace, vLen);
		     }
		     std::string err;
		     void* st = mach().takeState(err);
		     if (!st) throw std::runtime_error(err);
		     Machine& m = mach();
		     auto& mon = m.fdc();
		     const auto savedLog = mon.save();
		     const fdc::Breaks savedBreaks = mon.breaks();
		     const bool prof = m.profile().on, tr = m.trace().on, cov = m.coverage().on,
				beam = m.beamLog().on, aud = m.audio().on;
		     m.profile().on = m.trace().on = m.coverage().on = m.beamLog().on = m.audio().on = false;
		     mon.breaks() = fdc::Breaks();
		     int basePos[4] = {0, 0, 0, 0};
		     for (int i = 0; i < 4; i++) basePos[i] = c->dif->flp[i]->pos;
		     const int lenRef = c->dif->fdc->flp ? c->dif->fdc->flp->trklen : 6250;
		     json runs = json::array();
		     long long tMin = -1, tMax = -1, tSum = 0;
		     int reached = 0, worst = -1, best = -1, failed = 0;
		     std::string failure;
		     try {
			     for (int k = 0; k < n; k++) {
				     if (!m.putState(st, "sweep"))
					     throw std::runtime_error("the machine changed under the sweep");
				     for (int i = 0; i < 4; i++) {
					     Floppy* flp = c->dif->flp[i];
					     if (!flp->insert || flp->trklen <= 0) continue;
					     flp->pos = (int)((basePos[i] + (long long)k * flp->trklen / n) % flp->trklen);
					     flp->index = flp->pos < 4 ? 1 : 0;
				     }
				     mon.clear(c);
				     const RunResult r = m.run(kMaxInstructions, stopPc, maxFrames);
				     const fdc::Summary s = mon.summary();
				     const double deg = 360.0 * k / n;
				     json row = {{"angle_deg", std::round(deg * 10) / 10}, {"reached", r.reason == "pc"},
						 {"t", r.t}, {"frames", r.frames}, {"commands", s.commands},
						 {"sectors_ok", s.sectorsOk}, {"sectors_lost_data", s.sectorsLost},
						 {"revolutions", s.revolutions}};
				     if (s.crcErrors) row["crc_errors"] = s.crcErrors;
				     if (s.notFound) row["not_found"] = s.notFound;
				     if (r.reason != "pc") row["stopped"] = r.reason;
				     bool ok = r.reason == "pc";
				     if (doVerify) {
					     const json v = verifyMemory(diskIn(c, vDrive), vAddr, vLen, vPlace);
					     row["verify"] = v["ok"].get<bool>() ? json("ok") : v["first"];
					     ok = ok && v["ok"].get<bool>();
				     }
				     if (r.reason == "pc") {
					     reached++;
					     tSum += r.t;
					     if (tMin < 0 || r.t < tMin) { tMin = r.t; best = k; }
					     if (r.t > tMax) { tMax = r.t; worst = k; }
				     }
				     if (!ok) failed++;
				     runs.push_back(row);
			     }
		     } catch (const std::exception& e) {
			     failure = e.what();
		     }
		     const bool back = m.putState(st, "sweep");
		     m.freeState(st);
		     mon.restore(savedLog);
		     mon.breaks() = savedBreaks;
		     m.profile().on = prof; m.trace().on = tr; m.coverage().on = cov;
		     m.beamLog().on = beam; m.audio().on = aud;
		     if (!failure.empty()) throw std::runtime_error(failure);
		     json sum = {{"runs", n}, {"reached", reached}, {"failed", failed}};
		     if (reached) {
			     sum["t_min"] = tMin;
			     sum["t_max"] = tMax;
			     sum["t_mean"] = tSum / reached;
			     sum["best_angle_deg"] = std::round(3600.0 * best / n) / 10;
			     sum["worst_angle_deg"] = std::round(3600.0 * worst / n) / 10;
			     const Timing tm = timing(c);
			     if (tm.tPerFrame > 0)
				     sum["spread_frames"] = std::round((tMax - tMin) * 10.0 / tm.tPerFrame) / 10;
		     }
		     (void)lenRef;
		     json j = {{"summary", sum}, {"runs", runs}};
		     if (!back) j["warning"] = "the starting state could not be put back";
		     return j;
	     });

	tool("disk_track",
	     "The physical layout of one side of a cylinder as the controller sees it: sectors "
	     "in the order they pass the head, byte offsets of each ID and data field, size, "
	     "data mark, CRCs, the sector order, where the head is, T per byte and per revolution. "
	     "action fault puts a fault on a sector (kind: crc_data, crc_id, no_id, no_data, "
	     "deleted) and clear_faults takes every fault off the drive, byte for byte.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "layout (default) | fault | clear_faults"}}},
		     {"drive", {{"type", "integer"}}},
		     {"cylinder", {{"type", "integer"}, {"description", "default: where the head is"}}},
		     {"side", {{"type", "integer"}}},
		     {"sector", {{"type", "integer"}, {"description", "fault: the sector number in its ID"}}},
		     {"kind", {{"type", "string"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = bdi();
		     const int drv = argDrive(a);
		     Floppy* flp = diskIn(c, drv);
		     const std::string action = argStr(a, "action", "layout");
		     const int cyl = argNumRange(a, "cylinder", flp->trk, 0, 127);
		     const int side = argNumRange(a, "side", 0, 0, 1);
		     if (action == "clear_faults") {
			     const int n = mach().faults().clear(c->dif->flp, drv);
			     return json{{"drive", drv}, {"cleared", n}};
		     }
		     if (action == "fault") {
			     const int sec = argNumRange(a, "sector", -1, 0, 255);
			     if (sec < 0) throw std::runtime_error("sector required");
			     std::string err;
			     if (!mach().faults().add(flp, drv, cyl, side, sec, argStr(a, "kind"), err))
				     throw std::runtime_error(err);
		     } else if (action != "layout") {
			     throw std::runtime_error("unknown disk_track action '" + action + "'");
		     }
		     const fdc::TrackLayout lay = fdc::trackLayout(flp, cyl, side);
		     const double bt = byteT(c);
		     json secs = json::array();
		     std::string order;
		     long long dataBytes = 0;
		     for (const auto& s : lay.sectors) {
			     json e = {{"id", idJson(s.c, s.h, s.s, s.n)}, {"id_at", s.idAt},
				       {"id_crc", s.idCrcOk}, {"mark", markName(s.mark)}};
			     if (s.dataAt >= 0) {
				     e["data_at"] = s.dataAt;
				     e["size"] = s.size;
				     e["data_crc"] = s.dataCrcOk;
				     dataBytes += s.size;
			     }
			     const std::string f = mach().faults().kindAt(drv, cyl, side, s.s);
			     if (!f.empty()) e["fault"] = f;
			     secs.push_back(e);
			     order += (order.empty() ? "" : " ") + std::to_string(s.s);
		     }
		     // faults that took a sector off the track are listed too
		     json gone = json::array();
		     for (const auto& f : mach().faults().list(drv))
			     if (f.cyl == cyl && f.side == side && (f.kind == "no_id"))
				     gone.push_back({{"sector", f.sector}, {"fault", f.kind}});
		     json j = {{"drive", drv}, {"cylinder", cyl}, {"side", side}, {"trklen", lay.trklen},
			       {"byte_t", std::round(bt * 10) / 10}, {"revolution_t", roundT(bt * lay.trklen)},
			       {"order", order}, {"data_bytes", dataBytes}, {"sectors", secs}};
		     if (!gone.empty()) j["removed"] = gone;
		     const int headSide = (flp->doubleSide && c->dif->fdc->side) ? 1 : 0;
		     if (c->dif->fdc->flp == flp && flp->trk == cyl && headSide == side) j["head_at"] = flp->pos;
		     return j;
	     });

	tool("disk_sector",
	     "Sectors of the disk image, bypassing the controller. read: data as hex with its "
	     "ID, mark and CRCs (count up to 16). write: hex bytes into a sector, CRC recomputed. "
	     "verify: compare memory at address with the disk in TR-DOS order (sector 1..16, "
	     "side, cylinder) from a cylinder/side/sector or from a catalogue file; answers ok "
	     "or the first mismatch (address, sector, offset, both bytes) and how many differ.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "read | write | verify"}}},
		     {"drive", {{"type", "integer"}}},
		     {"cylinder", {{"type", "integer"}}},
		     {"side", {{"type", "integer"}}},
		     {"sector", {{"type", "integer"}, {"description", "as in its ID, 1..16 on TR-DOS"}}},
		     {"count", {{"type", "integer"}}},
		     {"hex", {{"type", "string"}}},
		     {"address", {{"type", "string"}, {"description", "verify: where the data was loaded"}}},
		     {"length", {{"type", "integer"}, {"description", "verify: default the file's length"}}},
		     {"file", {{"type", "string"}, {"description", "verify: NAME or NAME.C from the catalogue"}}}
	     }}, {"required", json::array({"action"})}},
	     [](const json& a) {
		     Computer* c = bdi();
		     const int drv = argDrive(a);
		     Floppy* flp = diskIn(c, drv);
		     const std::string action = argStr(a, "action");
		     if (action == "verify") {
			     const int address = argAddrRequired(a, "address");
			     int length = argNumOr(a, "length", -1);
			     Place p{0, 0, 1};
			     verifyStart(a, flp, p, length);
			     json j = verifyMemory(flp, address, length, p);
			     j["from"] = json::array({p.cyl, p.side, p.sec});
			     return j;
		     }
		     Place p;
		     p.cyl = argNumRange(a, "cylinder", -1, 0, 127);
		     p.side = argNumRange(a, "side", 0, 0, 1);
		     p.sec = argNumRange(a, "sector", -1, 0, 255);
		     if (p.cyl < 0 || p.sec < 0) throw std::runtime_error("cylinder and sector required");
		     if (action == "read") {
			     const int count = argNumRange(a, "count", 1, 1, 16);
			     json out = json::array();
			     for (int i = 0; i < count; i++) {
				     fdc::SectorLayout sl;
				     const auto d = sectorData(flp, p, &sl);
				     if (d.empty()) {
					     out.push_back({{"sector", json::array({p.cyl, p.side, p.sec})}, {"missing", true}});
				     } else {
					     out.push_back({{"sector", json::array({p.cyl, p.side, p.sec})},
							    {"id", idJson(sl.c, sl.h, sl.s, sl.n)},
							    {"mark", markName(sl.mark)},
							    {"id_crc", sl.idCrcOk}, {"data_crc", sl.dataCrcOk},
							    {"hex", hexBytes(d)}});
				     }
				     nextPlace(flp, p);
			     }
			     return json{{"drive", drv}, {"sectors", out}};
		     }
		     if (action == "write") {
			     const std::string hx = argStr(a, "hex");
			     std::vector<unsigned char> bytes;
			     std::string digits;
			     for (char ch : hx) if (!isspace((unsigned char)ch)) digits += ch;
			     if (digits.empty() || digits.size() % 2) throw std::runtime_error("hex must be whole bytes");
			     for (size_t i = 0; i < digits.size(); i += 2)
				     bytes.push_back(parseHexByte(digits.substr(i, 2), "hex"));
			     const fdc::TrackLayout lay = fdc::trackLayout(flp, p.cyl, p.side);
			     const fdc::SectorLayout* s = nullptr;
			     for (const auto& l : lay.sectors) if (l.s == p.sec && l.dataAt >= 0) { s = &l; break; }
			     if (!s) throw std::runtime_error("no such sector with a data field on the disk");
			     if ((int)bytes.size() > s->size)
				     throw std::runtime_error("the sector holds " + std::to_string(s->size) + " bytes");
			     unsigned char* b = flp->data[(p.cyl << 1) | p.side].byte;
			     const int len = lay.trklen;
			     for (size_t i = 0; i < bytes.size(); i++) b[(s->dataAt + (int)i) % len] = bytes[i];
			     std::vector<unsigned char> buf{(unsigned char)s->mark};
			     for (int i = 0; i < s->size; i++) buf.push_back(b[(s->dataAt + i) % len]);
			     const unsigned short crc = fdc::fieldCrc(buf.data(), (int)buf.size());
			     b[(s->dataAt + s->size) % len] = crc >> 8;
			     b[(s->dataAt + s->size + 1) % len] = crc & 0xff;
			     flp->changed = 1;
			     return json{{"drive", drv}, {"sector", json::array({p.cyl, p.side, p.sec})},
					 {"written", (int)bytes.size()}};
		     }
		     throw std::runtime_error("unknown disk_sector action '" + action + "' (read | write | verify)");
	     });

	tool("checkpoint",
	     "Whole-machine state kept in this server process: memory, CPU, paging, video and the "
	     "disk controller mid-command with the angle of the disk - what a .sna cannot hold, so "
	     "a load can be resumed from the middle. Not saved to a file and gone when the server "
	     "restarts; the disk contents are not part of it. action: save | load | list | drop "
	     "(drop without name drops all).",
	     json{{"properties", {
		     {"action", {{"type", "string"}}},
		     {"name", {{"type", "string"}}}
	     }}, {"required", json::array({"action"})}},
	     [](const json& a) {
		     const std::string action = argStr(a, "action");
		     const std::string name = argStr(a, "name");
		     std::string err;
		     if (action == "save") {
			     if (!mach().saveCheckpoint(name, err)) throw std::runtime_error(err);
			     size_t bytes = 0;
			     for (const auto& cp : mach().checkpoints()) if (cp.name == name) bytes = cp.bytes;
			     return json{{"saved", name}, {"bytes", bytes},
					 {"pc_hex", hex16(cpu_get_pc(mach().comp()->cpu))}};
		     }
		     if (action == "load") {
			     std::vector<std::string> warn;
			     if (!mach().loadCheckpoint(name, err, warn)) throw std::runtime_error(err);
			     json j = {{"loaded", name}, {"pc_hex", hex16(cpu_get_pc(mach().comp()->cpu))}};
			     if (!warn.empty()) j["warnings"] = warn;
			     return j;
		     }
		     if (action == "drop") return json{{"dropped", mach().dropCheckpoint(name)}};
		     if (action == "list") {
			     json out = json::array();
			     for (const auto& cp : mach().checkpoints())
				     out.push_back({{"name", cp.name}, {"model", cp.model}, {"frame", cp.frame},
						    {"bytes", cp.bytes}});
			     return json{{"checkpoints", out}};
		     }
		     throw std::runtime_error("unknown checkpoint action '" + action + "' (save | load | list | drop)");
	     });
}

} // namespace tools
} // namespace xsp
