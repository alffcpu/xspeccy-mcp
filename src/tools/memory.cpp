// Registers, memory, and the two directions of assembly.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerMemoryTools() {
	tool("get_registers",
	     "All CPU registers (main and shadow set), the flags string, IM/IFF and the T-state "
	     "counter for the current frame.",
	     json{},
	     [](const json&) {
		     Computer* c = mach().comp();
		     xRegBunch rb = cpuGetRegs(c->cpu);
		     json regs = json::object();
		     // The bunch ends at the first REG_EOT. Stopping on a NULL name instead
		     // reads uninitialised stack: cpuGetRegs() fills only as many entries as
		     // the CPU has registers and marks the end by setting `id` alone, leaving
		     // that entry's `name` whatever was on the stack (cpu/cpu.c).
		     for (int i = 0; i < 32 && rb.regs[i].id != REG_EOT; i++)
			     if (rb.regs[i].name) regs[rb.regs[i].name] = rb.regs[i].value;
		     return json{
			     {"registers", regs},
			     {"flags", rb.flags ? rb.flags : ""},
			     {"flags_value", cpu_get_flag(c->cpu)},
			     {"pc", cpu_get_pc(c->cpu)},
			     {"pc_hex", hex16(cpu_get_pc(c->cpu))},
			     {"sp", cpu_get_sp(c->cpu)},
			     {"halted", c->cpu->flgHALT ? true : false},
			     {"t_states_frame", c->frmtCount}
		     };
	     });

	tool("set_register",
	     "Set one register by name (PC, SP, AF, BC, DE, HL, IX, IY, A, B, ... as reported by get_registers).",
	     json{{"properties", {
		     {"name", {{"type", "string"}}},
		     {"value", {{"type", "string"}, {"description", "number, $hex/0x/#, or a label name"}}}
	     }}, {"required", json::array({"name", "value"})}},
	     [](const json& a) {
		     std::string name = argStr(a, "name");
		     int val = 0;
		     if (!argAddr(a, "value", val)) throw std::runtime_error("value required");
		     if (!cpu_set_reg(mach().comp()->cpu, name.c_str(), val))
			     throw std::runtime_error("unknown register '" + name + "'");
		     return json{{"name", name}, {"value", val}, {"ok", true}};
	     });

	tool("read_memory",
	     "Read bytes as the CPU sees them (through the current page mapping). An address of the "
	     "form \"bank:offset\" (both hex, as in a symbol file) reads that physical 16K RAM bank "
	     "directly - paged in or not; the offset is $0000..$3FFF and the read must end inside "
	     "the bank. Bank FF is the symbol file's \"no bank\": FF:8000 reads CPU address $8000. "
	     "The bytes come back as a hex string by default; format:\"bytes\" returns a decimal "
	     "array instead, format:\"both\" returns both.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg +
			 std::string("; \"BB:OOOO\" reads a physical RAM bank (FF = a CPU address)")}}},
		     {"length", {{"type", "integer"}, {"description", "default 16, max 16384"}}},
		     {"format", {{"type", "string"}, {"description", "hex (default) | bytes | both"}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     // required by the schema; the dispatch does not enforce it
		     if (!a.contains("address"))
			     throw std::runtime_error("address is required");
		     const std::string format = argStr(a, "format", "hex");
		     if (format != "hex" && format != "bytes" && format != "both")
			     throw std::runtime_error("format must be hex, bytes or both, not '" +
						      format + "'");
		     const bool wantHex = format != "bytes";
		     const bool wantArr = format != "hex";
		     // "bank:offset" is a physical page, not a CPU address: same two-hex-part
		     // spelling a symbol file uses, same bank unit breakpoints take - and
		     // bank FF is the symbol file's "no bank", a CPU address like any other
		     int adr = 0, bank = -1;
		     bool cpuAddr = true;
		     if (a["address"].is_string()) {
			     int b = -1, off = 0;
			     if (parseBankOffset(a["address"].get<std::string>(), b, off)) {
				     adr = off;
				     if (b >= 0) {
					     if (b >= mach().bankCount())
						     throw std::runtime_error("no RAM bank " + std::to_string(b) +
								      " (the machine has " +
								      std::to_string(mach().bankCount()) + ")");
					     bank = b;
				     }
				     cpuAddr = false;	// already read, and FF's number is in adr
			     }
		     }
		     const int len = argNumRange(a, "length", 16, 1, 16384);
		     json bytes = json::array();
		     std::string hex;
		     char buf[8];
		     auto collect = [&](const std::function<int(int)>& read) {
			     for (int i = 0; i < len; i++) {
				     const int b = read(i);
				     if (wantArr) bytes.push_back(b);
				     if (wantHex) {
					     snprintf(buf, sizeof(buf), "%02X", b);
					     if (i) hex += ' ';
					     hex += buf;
				     }
			     }
		     };
		     if (bank >= 0) {
			     // Past the end of the bank is refused, not continued: the next
			     // bank by number is not what comes next anywhere the CPU looks
			     // (bank 5 at $4000 is followed by bank 2), so reading on into
			     // it would hand back bytes from a neighbour the caller did not
			     // mean - and wrapping to the bank's own start is worse.
			     if (adr + len > 0x4000)
				     throw std::runtime_error("length " + std::to_string(len) + " from offset " +
							      hex16(adr) + " runs past the end of bank " +
							      std::to_string(bank) + " ($4000 bytes); read "
							      "the next bank with its own bank:offset");
			     collect([&](int i) { return mach().readBankByte(bank, adr + i); });
			     json res{{"bank", bank}, {"offset", adr}, {"offset_hex", hex16(adr)}};
			     if (wantArr) res["bytes"] = bytes;
			     if (wantHex) res["hex"] = hex;
			     return res;
		     }
		     if (cpuAddr) adr = argAddrRequired(a, "address");
		     collect([&](int i) { return mach().readByte(adr + i); });
		     json res{{"address", adr}, {"address_hex", hex16(adr)}};
		     if (wantArr) res["bytes"] = bytes;
		     if (wantHex) res["hex"] = hex;
		     return res;
	     });

	tool("write_memory",
	     "Write bytes through the CPU mapping. Writes to a ROM page are silently dropped by the "
	     "hardware - read back if it matters.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"bytes", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
		     {"hex", {{"type", "string"}, {"description", "alternative to bytes: \"00 3E 05\""}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     const int adr = argAddrRequired(a, "address");
		     std::vector<int> data;
		     if (a.contains("bytes") && a["bytes"].is_array()) {
			     for (const auto& b : a["bytes"]) {
				     if (!b.is_number_integer() || b.get<int>() < 0 || b.get<int>() > 255)
					     throw std::runtime_error("bad bytes: " + b.dump() +
								      " is not a byte (0..255)");
				     data.push_back(b.get<int>());
			     }
		     } else if (a.contains("hex")) {
			     for (const auto& t : xsp::split(argStr(a, "hex"), ' ')) {
				     std::string s = xsp::trim(t);
				     if (s.empty()) continue;
				     data.push_back(parseHexByte(s, "hex"));
			     }
		     } else {
			     throw std::runtime_error("give bytes[] or hex");
		     }
		     for (size_t i = 0; i < data.size(); i++)
			     mach().writeByte(adr + (int)i, data[i]);
		     return json{{"address", adr}, {"written", (int)data.size()}};
	     });

	tool("find_bytes",
	     "Search the CPU address space for a hex pattern (\"3E 05 C9\"). Returns the first "
	     "address or -1. Useful for finding an unlabelled routine by its opcodes, or for "
	     "locating data whose address moved after a rebuild.",
	     json{{"properties", {
		     {"pattern", {{"type", "string"}}},
		     {"from", {{"type", "string"}, {"description", kAddrArg}}},
		     {"to", {{"type", "string"}, {"description", kAddrArg}}}
	     }}, {"required", json::array({"pattern"})}},
	     [](const json& a) {
		     std::vector<unsigned char> pat;
		     for (const auto& t : xsp::split(argStr(a, "pattern"), ' ')) {
			     std::string s = xsp::trim(t);
			     if (!s.empty()) pat.push_back(parseHexByte(s, "pattern"));
		     }
		     if (pat.empty()) throw std::runtime_error("pattern is empty");
		     int from = argAddrRange(a, "from", 0);
		     int to = argAddrRange(a, "to", 0xffff);
		     int at = mach().findBytes(pat, from, to);
		     return json{{"address", at}, {"address_hex", at >= 0 ? hex16(at) : "-"}, {"found", at >= 0}};
	     });

	// -------------------------------------------------- code

	tool("disassemble",
	     "Disassemble from an address (default: PC). Each line gives the address, text, length, "
	     "raw bytes and, for jumps and calls, the target address.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", "default: current PC; label names work"}}},
		     {"count", {{"type", "integer"}, {"description", "instructions, default 16"}}},
		     {"t_states", {{"type", "boolean"}, {"description",
				   "add the T-states of each instruction and the total, default false"}}}
	     }}},
	     [](const json& a) {
		     int adr = cpu_get_pc(mach().comp()->cpu);
		     argAddr(a, "address", adr);
		     int count = argNumRange(a, "count", 16, 1, 4096);
		     if (count < 1) count = 1;
		     if (count > 256) count = 256;
		     const bool wantT = argBool(a, "t_states", false);
		     long long tMax = 0, tMin = 0;
		     bool anyCond = false;
		     json lines = json::array();
		     for (int i = 0; i < count; i++) {
			     int len = 0;
			     xMnem mn;
			     std::string text = mach().disasm(adr, &len, &mn);
			     if (len <= 0) len = 1;
			     std::string hex;
			     char buf[8];
			     for (int b = 0; b < len; b++) {
				     snprintf(buf, sizeof(buf), "%02X", mach().readByte(adr + b));
				     if (b) hex += ' ';
				     hex += buf;
			     }
			     // A listing row carries each fact once, in the spelling it is
			     // written back in: the address as $hex (the decimal twin was a
			     // quarter of every row), the length as the bytes themselves
			     json line{
				     {"address_hex", hex16(adr)},
				     {"text", text},
				     {"bytes", hex}
			     };
			     if (const std::string* lab = mach().labels().atAddress(adr))
				     line["label"] = *lab;
			     else if (const char* rom = fdc::trdosNameAt(mach().comp(), adr))
				     line["label"] = rom;	// only while the TR-DOS ROM is paged in
			     if (const auto* src = mach().listing().atAddress(adr))
				     line["source"] = src->text;
			     if (mn.oadr >= 0) {
				     line["target_hex"] = hex16(mn.oadr);
				     if (const std::string* lab = mach().labels().atAddress(mn.oadr))
					     line["target_label"] = *lab;
				     else if (const char* rom = fdc::trdosNameAt(mach().comp(), mn.oadr))
					     line["target_label"] = rom;
			     }
			     if (mn.cond) line["conditional"] = true;
			     if (wantT) {
				     xsp::InstrTiming t = mach().instrTiming(adr);
				     line["t_states"] = t.taken;
				     if (t.conditional) {
					     line["t_states_not_taken"] = t.not_taken;
					     line["t_states_text"] = std::to_string(t.taken) + "/" +
								     std::to_string(t.not_taken);
					     anyCond = true;
				     }
				     tMax += t.taken;
				     tMin += t.not_taken;
			     }
			     lines.push_back(line);
			     adr = (adr + len) & 0xffff;
		     }
		     json res{{"lines", lines}, {"next_address", adr}};
		     if (wantT) {
			     res["t_states_total"] = tMax;
			     if (anyCond) {
				     res["t_states_total_min"] = tMin;
				     res["note"] = "totals assume every conditional instruction "
						   "branches (total) or falls through (total_min); "
						   "straight-line code has one number";
			     }
		     }
		     return res;
	     });

	tool("assemble",
	     "Assemble Z80 mnemonics and (by default) write them to memory - write code as text "
	     "instead of hand-assembled bytes. Numbers may be decimal, 0x, $ or # (\"ld hl,$4000\"). "
	     "Define labels with \"loop:\" and jump to them by name, forwards or backwards "
	     "(\"djnz loop\", \"jr done\"); names from load_labels work too, and the labels this "
	     "block defined come back in `labels`. Directives are not supported. "
	     "Stops at the first line that fails.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"lines", {{"type", "array"}, {"items", {{"type", "string"}}}}},
		     {"text", {{"type", "string"}, {"description", "alternative to lines: newline-separated source"}}},
		     {"write", {{"type", "boolean"}, {"description", "write to memory, default true"}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     const int adr = argAddrRequired(a, "address");
		     std::vector<std::string> lines;
		     if (a.contains("lines") && a["lines"].is_array()) {
			     for (const auto& l : a["lines"]) lines.push_back(l.get<std::string>());
		     } else if (a.contains("text")) {
			     lines = xsp::split(argStr(a, "text"), '\n');
		     } else {
			     throw std::runtime_error("give lines[] or text");
		     }
		     std::map<std::string, int> defined;
		     auto res = mach().assemble(adr, lines, argBool(a, "write", true), &defined);
		     json out = json::array();
		     int next = adr;
		     bool ok = true;
		     for (const auto& l : res) {
			     std::string hex;
			     char buf[8];
			     for (size_t i = 0; i < l.bytes.size(); i++) {
				     snprintf(buf, sizeof(buf), "%02X", l.bytes[i]);
				     if (i) hex += ' ';
				     hex += buf;
			     }
			     json line{{"address_hex", hex16(l.address)},
				       {"text", l.text}, {"hex", hex}, {"ok", l.ok}};
			     if (!l.error.empty()) line["error"] = l.error;
			     out.push_back(line);
			     if (l.ok) next = l.address + (int)l.bytes.size();
			     else ok = false;
		     }
		     json res2{{"lines", out}, {"next_address", next}, {"ok", ok}};
		     if (!defined.empty()) {
			     json labels = json::object();
			     for (const auto& kv : defined)
				     labels[kv.first] = {{"address_hex", hex16(kv.second)}};
			     res2["labels"] = labels;
		     }
		     return res2;
	     });

	// -------------------------------------------------- breakpoints
}

} // namespace tools
} // namespace xsp
