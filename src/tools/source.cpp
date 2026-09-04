// Source-level work: symbols, listings, and which lines ran.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerSourceTools() {
	tool("load_labels",
	     "Load a symbol table so addresses have names. Understands the sjasmplus formats "
	     "(\"05:200E name\", \"FF:8000 name\", \"name: EQU $8000\", \"name = 32768\") - the same "
	     "file you would pass to xpeccy with -l. In the bank form the number is an offset inside "
	     "the bank, so bank 5 lands at $4000, bank 2 at $8000 and any other bank at $C000; bank FF "
	     "is a plain CPU address. Beware that sjasmplus writes EQU constants into the same file "
	     "and they are indistinguishable from addresses. Afterwards every address argument accepts "
	     "a label name, disassemble annotates lines and jump targets, and assemble takes labels as "
	     "operands.",
	     json{{"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string err;
		     if (!mach().labels().load(argStr(a, "path"), err)) throw std::runtime_error(err);
		     json sample = json::array();
		     json banks = json::object();
		     int n = 0;
		     for (const auto& e : mach().labels().all()) {
			     const std::string key = e.bank < 0 ? "cpu" : std::to_string(e.bank);
			     banks[key] = banks.value(key, 0) + 1;
			     if (n++ >= 10) continue;
			     sample.push_back({{"name", e.name}, {"address", e.address},
					       {"address_hex", hex16(e.address)}, {"bank", e.bank}});
		     }
		     return json{{"path", mach().labels().source()},
			     {"count", (int)mach().labels().size()},
			     {"by_bank", banks}, {"sample", sample}};
	     });

	tool("resolve_symbol",
	     "Look a symbol up by name, or find the label at an address. With neither argument, "
	     "returns the whole table.",
	     json{{"properties", {
		     {"name", {{"type", "string"}}},
		     {"address", {{"type", "string"}, {"description", kAddrArg}}}
	     }}},
	     [](const json& a) {
		     auto& lab = mach().labels();
		     if (a.contains("name")) {
			     std::string nm = argStr(a, "name");
			     const auto* e = lab.entry(nm);
			     if (!e) throw std::runtime_error("no such label '" + nm + "'");
			     return json{{"name", nm}, {"address", e->address},
				     {"address_hex", hex16(e->address)}, {"bank", e->bank}};
		     }
		     int adr = 0;
		     if (argAddr(a, "address", adr)) {
			     const std::string* nm = lab.atAddress(adr);
			     return json{{"address", adr}, {"address_hex", hex16(adr)},
				     {"name", nm ? *nm : ""}, {"found", nm != nullptr}};
		     }
		     json all = json::array();
		     for (const auto& e : lab.all())
			     all.push_back({{"name", e.name}, {"address", e.address},
					    {"address_hex", hex16(e.address)}, {"bank", e.bank}});
		     return json{{"count", (int)lab.size()}, {"labels", all}, {"source", lab.source()}};
	     });

	tool("load_listing",
	     "Load an assembler listing (sjasmplus .lst) to debug in terms of source lines instead of "
	     "addresses. Enables source_at, step_line and run_to_line, and makes disassemble show the "
	     "original source for each address.",
	     json{{"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string err;
		     if (!mach().listing().load(argStr(a, "path"), err)) throw std::runtime_error(err);
		     return json{{"path", mach().listing().source()},
			     {"lines", (int)mach().listing().size()}};
	     });

	tool("source_at",
	     "The source line at an address (default: PC), with a few lines of context either side - "
	     "what the programmer wrote, not what the disassembler reconstructs.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"context", {{"type", "integer"}, {"description", "lines each way, default 3"}}}
	     }}},
	     [](const json& a) {
		     int adr = cpu_get_pc(mach().comp()->cpu);
		     argAddr(a, "address", adr);
		     int ctx = argNumOr(a, "context", 3);
		     auto lines = mach().listing().around(adr, ctx, ctx);
		     if (lines.empty()) throw std::runtime_error("no listing line covers " + hex16(adr));
		     const auto* here = mach().listing().coveringAddress(adr);
		     json out = json::array();
		     for (const auto& l : lines)
			     out.push_back({{"address", l.address}, {"address_hex", hex16(l.address)},
					    {"line", l.line}, {"file", l.file}, {"text", l.text},
					    {"current", here && l.line == here->line && l.file == here->file}});
		     return json{{"address", adr}, {"address_hex", hex16(adr)},
			     {"line", here ? here->line : -1}, {"file", here ? here->file : ""},
			     {"source", out}};
	     });

	tool("step_line",
	     "Step until execution reaches a different source line - one source step instead of one "
	     "instruction. Needs a listing (load_listing); without one it falls back to a single step.",
	     json{},
	     [](const json&) {
		     auto r = runToJson(mach().stepLine());
		     if (const auto* l = mach().listing().coveringAddress(cpu_get_pc(mach().comp()->cpu))) {
			     r["line"] = l->line;
			     r["file"] = l->file;
			     r["text"] = l->text;
		     }
		     return r;
	     });

	tool("run_to_line",
	     "Run until a given source line in the listing is reached. Needs load_listing first, "
	     "and the line must be one that generated code - a comment or an equate has no address "
	     "to stop at.",
	     json{{"properties", {
		     {"line", {{"type", "integer"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "budget, default 10000000"}}}
	     }}, {"required", json::array({"line"})}},
	     [](const json& a) {
		     int line = argNumOr(a, "line", 0);
		     int adr = 0;
		     if (!mach().listing().addressOfLine(line, adr))
			     throw std::runtime_error("line " + std::to_string(line) + " has no code in the listing");
		     const long long budget = argNumRange(a, "max_instructions", 10000000,
							  1, (int)kMaxInstructions);
		     auto r = runToJson(mach().run(budget, adr, -1));
		     r["target"] = adr;
		     r["target_hex"] = hex16(adr);
		     r["line"] = line;
		     return r;
	     });

	// -------------------------------------------------- coverage

	tool("coverage",
	     "Which bytes of code actually ran. action=start begins recording, read reports the "
	     "executed fraction of a range and - more usefully - the biggest stretches that never "
	     "executed, so you can see which branches your test never took.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "start | stop | read | reset"}}},
		     {"from", {{"type", "string"}, {"description", "range start, default $4000; label names work"}}},
		     {"to", {{"type", "string"}, {"description", "range end, default $FFFF; label names work"}}},
		     {"max_gaps", {{"type", "integer"}, {"description", "unexecuted stretches to list, default 10"}}},
		     {"min_gap", {{"type", "integer"}, {"description", "ignore gaps shorter than this, default 4"}}}
	     }}},
	     [](const json& a) {
		     auto& cov = mach().coverage();
		     std::string action = argStr(a, "action", "read");
		     if (action == "start") { cov.reset(); cov.on = true; }
		     else if (action == "stop") cov.on = false;
		     else if (action == "reset") cov.reset();
		     else if (action != "read") throw std::runtime_error("action must be start, stop, read or reset");

		     // Both ends checked before the swap, not after: clamping one end
		     // and then swapping used to walk the map from the surviving end
		     // out to the unchecked one, which reads past the array and
		     // reports a byte count for memory that does not exist.
		     int from = argAddrRange(a, "from", 0x4000);
		     int to = argAddrRange(a, "to", 0xffff);
		     if (to < from) std::swap(from, to);

		     int executed = 0;
		     for (int i = from; i <= to; i++) if (cov.map[i] & 1) executed++;

		     int minGap = argNumOr(a, "min_gap", 4);
		     int maxGaps = argNumRange(a, "max_gaps", 10, 0, 4096);
		     std::vector<std::pair<int, int>> gaps;		// (size, start)
		     int gapStart = -1;
		     for (int i = from; i <= to + 1; i++) {
			     bool run = (i <= to) && !(cov.map[i] & 1);
			     if (run && gapStart < 0) gapStart = i;
			     if (!run && gapStart >= 0) {
				     int size = i - gapStart;
				     if (size >= minGap) gaps.push_back({size, gapStart});
				     gapStart = -1;
			     }
		     }
		     std::sort(gaps.begin(), gaps.end(), std::greater<std::pair<int, int>>());
		     json out = json::array();
		     for (int i = 0; i < (int)gaps.size() && i < maxGaps; i++) {
			     json g{{"start", gaps[i].second}, {"start_hex", hex16(gaps[i].second)},
				    {"size", gaps[i].first},
				    {"end_hex", hex16(gaps[i].second + gaps[i].first - 1)}};
			     if (const std::string* lab = mach().labels().atAddress(gaps[i].second))
				     g["label"] = *lab;
			     out.push_back(g);
		     }
		     const int total = to - from + 1;
		     return json{
			     {"running", cov.on},
			     {"instructions", cov.instructions},
			     {"range", {{"from", from}, {"to", to}}},
			     {"bytes_executed", executed},
			     {"bytes_total", total},
			     {"percent", total ? 100.0 * executed / total : 0.0},
			     {"unexecuted_gaps", out},
			     {"gaps_total", (int)gaps.size()}
		     };
	     });

	// -------------------------------------------------- raster and timing
}

} // namespace tools
} // namespace xsp
