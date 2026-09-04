// Where the T-states went, and where the beam was when they went there.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerTimingTools() {
	tool("beam_position",
	     "Where the video beam is right now: dot and line inside the full frame, which zone it is "
	     "in (paper, border or blanking), and the T-state count since the interrupt. This is what "
	     "you need when debugging multicolour, border effects or anything raster-timed.\n"
	     "Three coordinate systems describe the same beam and the reply carries all of them: "
	     "dot/line count from the top-left of the visible image, paper_x/paper_y from the corner "
	     "of the main screen, and blank_x/blank_y from the leading edge of the blanking. The last "
	     "one is not decoration: the frame interrupt is defined in it, and `interrupt_at` gives "
	     "its position, so blank_x/blank_y against interrupt_at is how far the beam is from INT.\n"
	     "`screen_page` is the RAM page the ULA is reading, which is bit 3 of $7FFD. Code that "
	     "flips screens between frames moves that and nothing else.",
	     json{},
	     [](const json&) {
		     Computer* c = mach().comp();
		     Video* v = c->vid;
		     int x = v->ray.x, y = v->ray.y;
		     std::string zone = "blank";
		     bool inPaperX = x >= v->bord.x && x < v->send.x;
		     bool inPaperY = y >= v->bord.y && y < v->send.y;
		     if (x < v->vend.x && y < v->vend.y)
			     zone = (inPaperX && inPaperY) ? "paper" : "border";
		     Timing tm = timing(c);
		     return json{
			     {"dot", x}, {"line", y}, {"zone", zone},
			     {"paper_x", inPaperX ? x - v->bord.x : -1},
			     {"paper_y", inPaperY ? y - v->bord.y : -1},
			     {"blank_x", v->ray.xb}, {"blank_y", v->ray.yb},
			     {"interrupt_at", {{"blank_x", v->intp.x}, {"blank_y", v->intp.y}}},
			     {"screen_page", v->vidPage},
			     {"t_states_frame", c->frmtCount},
			     {"t_states_per_frame", tm.tPerFrame},
			     {"t_states_per_line", tm.tPerLine},
			     {"frame_counter", v->fcnt},
			     {"vblank", v->vblank ? true : false},
			     {"hblank", v->hblank ? true : false}
		     };
	     });

	tool("frame_timing",
	     "Frame geometry and timing budget: dots and lines, T-states per line and per frame, "
	     "the paper/border/blanking split, and how many T-states the last frame actually took. "
	     "Budget an effect against t_states_per_frame and fps_real - the clock divided by the "
	     "T-states in a frame, 48.83 Hz on a Pentagon rather than the round 50 it is usually "
	     "quoted at. The ns_per_* fields are the core's internal quantum, which is a truncated "
	     "integer; the *_real ones come from the CPU clock and are what a real machine does.",
	     json{},
	     [](const json&) {
		     Computer* c = mach().comp();
		     Video* v = c->vid;
		     Timing tm = timing(c);
		     json out = {
			     {"fps_real", tm.fps},
			     {"cpu_frq_mhz", c->cpuFrq},
			     {"t_states_per_frame", tm.tPerFrame},
			     {"t_states_per_line", tm.tPerLine},
			     {"t_states_last_frame", c->fCount},
			     {"t_states_this_frame", c->frmtCount},
			     {"ns_per_frame", v->nsPerFrame},
			     {"ns_per_frame_real", tm.nsPerFrame},
			     {"ns_per_line", v->nsPerLine},
			     {"ns_per_dot", v->nsPerDot},
			     {"ns_per_t_state", c->nsPerTick},
			     {"ns_per_t_state_real", tm.nsPerT},
			     {"geometry", {
				     {"full", {{"x", v->full.x}, {"y", v->full.y}}},
				     {"blank", {{"x", v->blank.x}, {"y", v->blank.y}}},
				     {"border", {{"x", v->bord.x}, {"y", v->bord.y}}},
				     {"paper", {{"x", v->scrn.x}, {"y", v->scrn.y}}},
				     {"visible", {{"x", v->vsze.x}, {"y", v->vsze.y}}}
			     }},
			     {"interrupt", {{"position", v->intp.y}, {"length_t", v->intsize}}}
		     };
		     std::string warn = geometryWarning(tm);
		     if (!warn.empty()) out["warning"] = warn;
		     return out;
	     });

	tool("frame_cost",
	     "What one frame of an effect costs, in T-states. run_frames counts interrupts, which is "
	     "the wrong unit as soon as a render overruns one: this measures the work between two "
	     "frame boundaries - the CPU entering HALT by default - and keeps the time parked in HALT "
	     "separate, so work + idle is the wall clock and idle is exactly the headroom. Reports "
	     "every frame plus min/max/avg, how many interrupts a frame takes (the quantum), the "
	     "budget that many interrupts give, the headroom and the frame rate that follows. "
	     "Position the machine first (a breakpoint on the main loop, or run_frames); the partial "
	     "frame up to the first boundary is measured and reported separately, not averaged in.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "effect frames to measure, default 8"}}},
		     {"sync", {{"type", "string"}, {"description", "halt (default) | an address/label the frame ends at"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before measuring - the precalculation "
				     "otherwise lands in alignment_t"}}},
		     {"skip_max_instructions", {{"type", "integer"}, {"description",
				     "budget for the skip itself, default 200000000"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "per-frame budget, default 20000000"}}}
	     }}},
	     [](const json& a) {
		     const int frames = argNumRange(a, "frames", 8, 1, 10000);
		     json skipped = skipUntil(a);
		     SyncSpec sync = argSync(a);
		     if (sync.byInterrupt)
			     throw std::runtime_error("sync=frame measures a hardware frame, which is "
						      "always t_states_per_frame; use halt or an address");
		     const long long budget = argNumRange(a, "max_instructions", 20000000,
							  1, (int)kMaxInstructions);
		     Computer* c = mach().comp();
		     Timing tm = timing(c);
		     const long long nsPerT = c->nsPerTick > 0 ? c->nsPerTick : 1;

		     // land on a boundary first, so frame 1 is a whole frame. The
		     // profiler must not see this one: it is a partial frame, and
		     // counting it would skew a profile taken in the same pass.
		     xsp::FrameCost align = mach().frameCost(sync.pc, budget, false);
		     if (!align.complete)
			     throw std::runtime_error("no frame boundary (" + sync.text + ") within " +
						      std::to_string(budget) + " instructions - is the "
						      "machine running the effect?");

		     json list = json::array();
		     long long mn = -1, mx = 0, sum = 0;
		     for (int i = 0; i < frames; i++) {
			     xsp::FrameCost fc = mach().frameCost(sync.pc, budget);
			     if (!fc.complete) break;
			     long long work = fc.work_ns / nsPerT;
			     long long idle = fc.idle_ns / nsPerT;
			     if (mn < 0 || work < mn) mn = work;
			     if (work > mx) mx = work;
			     sum += work;
			     list.push_back({{"frame", i}, {"t_states", work}, {"idle_t", idle},
					     {"total_t", work + idle}, {"interrupts", fc.interrupts},
					     {"instructions", fc.instructions}});
		     }
		     if (list.empty())
			     throw std::runtime_error("no complete frame within the budget");

		     const int perInt = tm.tPerFrame > 0 ? tm.tPerFrame : 1;
		     const long long q = (mx + perInt - 1) / perInt;		// ceil
		     const long long budgetT = q * perInt;
		     json out{
			     {"sync", sync.text},
			     {"frames", (int)list.size()},
			     {"frame_list", list},
			     {"alignment_t", align.work_ns / nsPerT},
			     {"t_states_min", mn},
			     {"t_states_max", mx},
			     {"t_states_avg", (double)sum / (double)list.size()},
			     {"t_states_per_interrupt", perInt},
			     {"interrupts_per_frame", q},
			     {"budget_t", budgetT},
			     {"headroom_t", budgetT - mx},
			     {"headroom_percent", budgetT ? 100.0 * (budgetT - mx) / budgetT : 0.0},
			     // effect_fps is the effect's own rate; frame_timing's fps_real is
			     // the machine's. Same word, different quantities, so this one says
			     // which it is. `fps` stays as it was.
			     {"effect_fps", q ? tm.fps / (double)q : 0.0},
			     {"fps", q ? tm.fps / (double)q : 0.0}
		     };
		     if (!skipped.is_null()) out["skipped_to"] = skipped;
		     // the budget and the headroom are only as right as the frame length
		     std::string warn = geometryWarning(tm);
		     if (!warn.empty()) out["warning"] = warn;
		     return out;
	     });

	tool("profile",
	     "Instruction and T-state profiler. action=start begins counting (optionally over named "
	     "address ranges), read reports where the CPU spent its time - both per named range and as "
	     "a histogram of the busiest 256-byte pages - stop and reset do what they say. Counting "
	     "covers run/run_frames/step. Each range reports its share of the frame budget, how many "
	     "times control entered it (calls) and what one call cost (t_per_call) - which is the "
	     "number you optimise against. t_states and t_per_call are self time: what ran with the "
	     "PC inside the range, with a callee's time belonging to the callee. The *_inclusive "
	     "figures beside them add everything the range called out to, which is what \"what does "
	     "this routine cost me\" means for anything that is not a leaf. ranges:\"symbols\" splits by the loaded label table "
	     "instead, one region per label up to the next, which is the first thing to do on code "
	     "you do not know.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "start | stop | read | reset"}}},
		     {"ranges", {{"description",
				 "[{tag,start,end}] to measure separately. start and end are addresses "
				 "or label names and both are required; the region is [start, end), so "
				 "naming the next label as end measures exactly one routine. Or the "
				 "string \"symbols\" to split by every non-local label"}}},
		     {"from", {{"type", "string"}, {"description", "symbols mode: lowest address, default $4000"}}},
		     {"to", {{"type", "string"}, {"description", "symbols mode: highest address, default $FFFF"}}},
		     {"max_ranges", {{"type", "integer"}, {"description", "symbols mode: regions to report, default 24"}}},
		     {"top", {{"type", "integer"}, {"description", "how many hot pages to report, default 8"}}}
	     }}},
	     [](const json& a) {
		     auto& p = mach().profile();
		     std::string action = argStr(a, "action", "read");
		     static bool bySymbols = false;
		     // A start that failed leaves the previous profile in place, and its
		     // numbers look like a perfectly good measurement of the ranges you
		     // thought you had just asked for. Remember the failure so read can
		     // say so - this is the case that costs an afternoon.
		     static std::string lastStartError;
		     if (action == "start") {
			     try {
			     // Ranges go into a temporary and are committed only once every
			     // bound has resolved. A start that throws half way through must
			     // not leave the profiler holding a mangled set of ranges that a
			     // later read would then present as if they were measurements.
			     std::vector<xsp::Profile::Range> next;
			     bool nextBySymbols = false;
			     if (a.contains("ranges") && a["ranges"].is_string()) {
				     std::string mode = xsp::trim(a["ranges"].get<std::string>());
				     if (mode != "symbols")
					     throw std::runtime_error("ranges must be an array of "
								      "{tag,start,end} or the string \"symbols\"");
				     if (!mach().labels().size())
					     throw std::runtime_error("ranges=\"symbols\" needs a symbol "
								      "table - run load_labels first");
				     const int from = argAddrRange(a, "from", 0x4000);
				     const int to = argAddrRange(a, "to", 0xffff);
				     // one region per non-local label, up to the next one. Local
				     // labels (name.sub) would cut routines into fragments, and
				     // SMC labels declared as "name equ $+1" sit inside an
				     // instruction, so only whole names are used as boundaries.
				     std::vector<std::pair<int, std::string>> pts;
				     for (const auto& e : mach().labels().all()) {
					     if (e.name.find('.') != std::string::npos) continue;
					     if (e.address < from || e.address > to) continue;
					     pts.push_back({e.address, e.name});
				     }
				     if (pts.empty())
					     throw std::runtime_error("no non-local labels between " +
								      hex16(from) + " and " + hex16(to));
				     for (size_t i = 0; i < pts.size(); i++) {
					     xsp::Profile::Range range;
					     range.tag = pts[i].second;
					     range.start = pts[i].first;
					     range.end = (i + 1 < pts.size()) ? pts[i + 1].first : to + 1;
					     if (range.end > range.start) next.push_back(range);
				     }
				     nextBySymbols = true;
			     } else if (a.contains("ranges") && a["ranges"].is_array()) {
				     for (const auto& r : a["ranges"]) {
					     xsp::Profile::Range range;
					     range.tag = r.value("tag", std::string("range"));
					     // an unresolvable bound must fail here: a range left at
					     // 0..0 counts nothing and reads as "never executed"
					     if (!argAddr(r, "start", range.start) ||
						 !argAddr(r, "end", range.end))
						     throw std::runtime_error("range '" + range.tag +
							     "' needs both start and end");
					     if (range.end < range.start) std::swap(range.start, range.end);
					     next.push_back(range);
				     }
				     nextBySymbols = false;
			     } else {
				     next = p.ranges;		// start again over the same regions
				     nextBySymbols = bySymbols;
			     }
			     p.ranges = next;			// nothing above threw: commit
			     bySymbols = nextBySymbols;
			     p.reset();
			     p.on = true;
			     p.armed = true;
			     lastStartError.clear();
			     } catch (const std::exception& e) {
				     lastStartError = e.what();
				     throw;
			     }
		     } else if (action == "stop") {
			     p.on = false;
		     } else if (action == "reset") {
			     p.reset();
		     } else if (action != "read") {
			     throw std::runtime_error("action must be start, stop, read or reset");
		     }

		     int top = argNumOr(a, "top", 8);
		     std::vector<std::pair<long long, int>> pages;
		     for (int i = 0; i < 256; i++)
			     if (p.page[i]) pages.push_back({p.page[i], i});
		     std::sort(pages.begin(), pages.end(), std::greater<std::pair<long long, int>>());
		     json hot = json::array();
		     for (int i = 0; i < (int)pages.size() && i < top; i++) {
			     int page = pages[i].second;
			     json e{{"page", hex16(page << 8)}, {"instructions", pages[i].first}};
			     if (p.instructions)
				     e["percent"] = 100.0 * pages[i].first / p.instructions;
			     hot.push_back(e);
		     }
		     Computer* c = mach().comp();
		     Timing tm = timing(c);
		     const long long nsPerT = c->nsPerTick > 0 ? c->nsPerTick : 0;
		     const long long totalT = nsPerT ? p.ns / nsPerT : 0;

		     // symbols mode makes one region per label, most of them empty:
		     // report the ones that ran, dearest first
		     std::vector<const xsp::Profile::Range*> order;
		     for (const auto& r : p.ranges)
			     if (!bySymbols || r.instructions) order.push_back(&r);
		     if (bySymbols)
			     std::sort(order.begin(), order.end(),
				       [](const xsp::Profile::Range* x, const xsp::Profile::Range* y) {
					       return x->ns > y->ns;
				       });
		     const int maxRanges = argNumRange(a, "max_ranges", 24, 1, 4096);

		     json ranges = json::array();
		     for (size_t i = 0; i < order.size(); i++) {
			     if (bySymbols && (int)i >= maxRanges) break;
			     const auto& r = *order[i];
			     json e{{"tag", r.tag}, {"start", r.start}, {"end", r.end},
				    {"start_hex", hex16(r.start)}, {"end_hex", hex16(r.end)},
				    {"last_hex", hex16(r.end - 1)},	// end is exclusive
				    {"instructions", r.instructions}, {"ns", r.ns},
				    {"calls", r.calls}, {"entries", r.entries}};
			     long long t = nsPerT ? r.ns / nsPerT : 0;
			     if (nsPerT) e["t_states"] = t;
			     if (r.calls) e["t_per_call"] = (double)t / (double)r.calls;
			     // self time is what ran in here; inclusive adds what it called
			     // out to, which is what "what does this routine cost me" means
			     // for anything that is not a leaf
			     if (nsPerT) {
				     long long ti = r.inclusiveNs / nsPerT;
				     e["t_states_inclusive"] = ti;
				     e["instructions_inclusive"] = r.inclusiveInstructions;
				     if (r.calls) e["t_per_call_inclusive"] = (double)ti / (double)r.calls;
			     }
			     // a region whose first address never executed is usually a data
			     // label that swallowed the code lying after it
			     if (r.instructions && !r.startHits) e["start_never_executed"] = true;
			     if (totalT) e["percent"] = 100.0 * t / totalT;
			     if (p.frames > 0) {
				     double perFrame = (double)t / p.frames;
				     e["t_states_per_frame"] = perFrame;
				     if (tm.tPerFrame > 0)
					     e["percent_of_frame"] = 100.0 * perFrame / tm.tPerFrame;
			     }
			     ranges.push_back(e);
		     }
		     json res{
			     {"armed", p.armed},
			     {"running", p.on},
			     {"instructions", p.instructions},
			     {"frames", p.frames},
			     {"ns", p.ns},
			     {"t_states", totalT},
			     {"t_states_per_frame", tm.tPerFrame},	// the budget a range is measured against
			     {"fps_real", tm.fps},
			     {"hot_pages", hot},
			     {"ranges", ranges}
		     };
		     // Zeros from a profiler that was never started look exactly like
		     // zeros from code that never ran, and reading them as the second is
		     // an expensive mistake. Say which it is.
		     if (!lastStartError.empty()) {
			     res["last_start_failed"] = lastStartError;
			     res["warning"] = "the last profile start FAILED (" + lastStartError +
					      "), so nothing here describes the ranges you asked for - "
					      "it is either the profile from before that call or nothing "
					      "at all. Fix the start and run it again.";
		     } else if (!p.armed)
			     res["warning"] = "the profiler has never been started, so every number "
					      "here is zero because nothing was measured - not because "
					      "nothing ran. Call profile {\"action\":\"start\"} first, and "
					      "check that it succeeded: one bad label name in ranges "
					      "aborts the whole start.";
		     else if (!p.instructions)
			     res["warning"] = "the profiler is armed but no instruction has been "
					      "executed since it started - run, run_frames, step or "
					      "frame_cost feed it.";
		     if (!p.ranges.empty()) {
			     res["ranges_measured"] = (int)p.ranges.size();
			     res["ranges_reported"] = (int)ranges.size();
			     json other{{"instructions", p.otherInstructions},
					{"t_states", nsPerT ? p.otherNs / nsPerT : 0}};
			     if (totalT)
				     other["percent"] = 100.0 * (nsPerT ? p.otherNs / nsPerT : 0) / totalT;
			     res["unattributed"] = other;	// time in no range at all
		     }
		     return res;
	     });

	tool("beam_log",
	     "Record where the beam was every time a given address executed, without stopping. This "
	     "is the tool for raster timing: a breakpoint answers \"where is the beam this once\", and "
	     "the question raster code actually raises is whether the answer is the same on every "
	     "frame. Watch the entry of an interrupt handler, or the OUT that flips a bank, run a few "
	     "dozen frames, and the spread in t_states_frame is the jitter - which is usually the "
	     "whole diagnosis. The summary reports it per address without reading a single event: "
	     "hits are numbered within their frame and compared position by position across frames, "
	     "because a strip loop hits its OUT dozens of times a frame and a plain min/max over "
	     "every hit only restates that the loop spans the frame. `jitter_shape` says whether "
	     "every position drifts by the same amount, which is a constant offset of the whole "
	     "pass, or by different amounts, which is drift accumulating inside it. "
	     "action: enable, disable, clear, dump.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "enable | disable | clear | dump (default)"}}},
		     {"addresses", {{"type", "array"}, {"description",
				    "addresses or labels to watch, when enabling"},
				    {"items", {{"type", "string"}}}}},
		     {"count", {{"type", "integer"}, {"description", "events to dump, default 64"}}},
		     {"summary", {{"type", "boolean"}, {"description",
				  "per-address jitter across frames, default true"}}},
		     {"by_position", {{"type", "boolean"}, {"description",
				      "add the per-position breakdown; off by default because a "
				      "48-strip loop makes 48 entries and jitter_shape already "
				      "says whether they agree"}}},
		     {"size", {{"type", "integer"}, {"description", "ring size when enabling, default 4096"}}}
	     }}},
	     [](const json& a) {
		     auto& rl = mach().beamLog();
		     const std::string action = argStr(a, "action", "dump");
		     if (action == "enable") {
			     if (!a.contains("addresses") || !a["addresses"].is_array() ||
				 a["addresses"].empty())
				     throw std::runtime_error("enable needs addresses[]");
			     rl.reset((size_t)argNumRange(a, "size", 4096, 1, 1 << 20));
			     rl.clearWatch();
			     for (const auto& item : a["addresses"]) {
				     int adr = 0;
				     json one = json::object();
				     one["a"] = item;
				     if (!argAddr(one, "a", adr))
					     throw std::runtime_error("bad address: " + item.dump());
				     if (adr < 0 || adr > 0xffff)
					     throw std::runtime_error("address outside $0000..$FFFF: " +
								      item.dump());
				     rl.addWatch(adr);
			     }
			     rl.on = true;
		     } else if (action == "disable") {
			     rl.on = false;
		     } else if (action == "clear") {
			     rl.reset(rl.capacity);
		     } else if (action != "dump") {
			     throw std::runtime_error("action must be enable, disable, clear or dump");
		     }

		     json watched = json::array();
		     for (int adr : rl.watched()) {
			     json w{{"address", adr}, {"address_hex", hex16(adr)}};
			     if (const std::string* lab = mach().labels().atAddress(adr))
				     w["label"] = *lab;
			     watched.push_back(w);
		     }
		     json res{{"running", rl.on}, {"capacity", (int)rl.capacity},
			      {"events_total", rl.total}, {"watching", watched}};

		     if (action == "dump") {
			     std::vector<xsp::BeamLog::Event> evs =
				     rl.dump((size_t)argNumRange(a, "count", 64, 1, 1 << 20));
			     json list = json::array();
			     for (const auto& e : evs)
				     list.push_back({{"address", e.pc}, {"address_hex", hex16(e.pc)},
						     {"frame_counter", e.frame},
						     {"t_states_frame", e.t},
						     {"line", e.line}, {"dot", e.dot}});
			     res["events"] = list;
			     if (argBool(a, "summary", true)) {
				     // Over the whole ring, not just the dumped tail: the spread
				     // is the answer here and it should not depend on `count`.
				     //
				     // Raster code runs the same address many times per frame -
				     // a multicolour strip loop hits its OUT once per band - so
				     // the min and max over every hit merely restate that the
				     // loop spans the frame. Jitter is the Nth hit of one frame
				     // against the Nth hit of the next, and that is what is
				     // measured here: hits are numbered within their frame and
				     // compared across frames position by position.
				     std::vector<xsp::BeamLog::Event> all = rl.dump(rl.capacity);
				     // The ring usually begins mid-frame, and that frame is
				     // missing its early hits, which would fake a huge spread on
				     // every position. Start at the first frame boundary we saw.
				     size_t begin = 0;
				     if (!all.empty()) {
					     const int f0 = all[0].frame;
					     while (begin < all.size() && all[begin].frame == f0) begin++;
				     }
				     struct Occ { int lo, hi; long long n; };
				     std::map<std::pair<int, int>, Occ> byOcc;	// (pc, nth in frame)
				     std::map<int, long long> hits;
				     std::map<int, std::map<int, int>> perFrame;	// pc -> frame -> count
				     std::map<int, int> nth;
				     int curFrame = begin < all.size() ? all[begin].frame : 0;
				     for (size_t i = begin; i < all.size(); i++) {
					     const auto& e = all[i];
					     if (e.frame != curFrame) { curFrame = e.frame; nth.clear(); }
					     const int k = nth[e.pc]++;
					     hits[e.pc]++;
					     perFrame[e.pc][e.frame]++;
					     auto key = std::make_pair(e.pc, k);
					     auto it = byOcc.find(key);
					     if (it == byOcc.end()) byOcc.emplace(key, Occ{e.t, e.t, 1});
					     else {
						     Occ& o = it->second;
						     if (e.t < o.lo) o.lo = e.t;
						     if (e.t > o.hi) o.hi = e.t;
						     o.n++;
					     }
				     }
				     const bool wantPos = argBool(a, "by_position", false);
				     json sum = json::array();
				     for (const auto& kv : hits) {
					     const int pc = kv.first;
					     int worst = 0, worstAt = -1, positions = 0, best = -1;
					     json byPos = json::array();
					     for (const auto& oc : byOcc) {
						     if (oc.first.first != pc) continue;
						     positions++;
						     const int spread = oc.second.hi - oc.second.lo;
						     if (spread > worst) { worst = spread; worstAt = oc.first.second; }
						     if (best < 0 || spread < best) best = spread;
						     if (wantPos && byPos.size() < 512)
							     byPos.push_back({{"nth_in_frame", oc.first.second},
									      {"frames", oc.second.n},
									      {"t_min", oc.second.lo},
									      {"t_max", oc.second.hi},
									      {"jitter_t", spread}});
					     }
					     int pfLo = -1, pfHi = -1;
					     for (const auto& f : perFrame[pc]) {
						     if (pfLo < 0 || f.second < pfLo) pfLo = f.second;
						     if (f.second > pfHi) pfHi = f.second;
					     }
					     // Whether the jitter is the same on every position or
					     // grows along them is the whole difference between a
					     // constant offset of the pass and drift accumulating
					     // inside it. That is two numbers, not one entry per
					     // position, and a 48-strip loop has 48 of those.
					     json s{{"address", pc}, {"address_hex", hex16(pc)},
						    {"hits", kv.second},
						    {"frames", (long long)perFrame[pc].size()},
						    {"hits_per_frame_min", pfLo},
						    {"hits_per_frame_max", pfHi},
						    {"jitter_t", worst},
						    {"jitter_t_least", best < 0 ? 0 : best},
						    {"jitter_worst_nth_in_frame", worstAt},
						    {"positions_per_frame", positions},
						    {"jitter_shape", positions < 2 ? "single"
								    : (best == worst ? "uniform" : "varies")}};
					     if (wantPos) s["by_position"] = byPos;
					     if (pfLo != pfHi)
						     s["warning"] = "the number of hits per frame is not "
								    "constant, so positions do not line up "
								    "between frames and jitter_t is unreliable";
					     if (const std::string* lab = mach().labels().atAddress(pc))
						     s["label"] = *lab;
					     sum.push_back(s);
				     }
				     res["summary"] = sum;
				     if (begin >= all.size() && !all.empty())
					     res["summary_note"] = "every event in the ring belongs to one "
								   "frame; run more frames, or raise `size`, "
								   "to compare frames against each other";
			     }
		     }
		     return res;
	     });

	// -------------------------------------------------- sound
}

} // namespace tools
} // namespace xsp
