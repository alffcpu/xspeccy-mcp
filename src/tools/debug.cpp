// Breakpoints and tracing: stopping somewhere, and seeing how it got there.
#include "tools/common.h"

namespace xsp {
namespace tools {

// "exec", "read", "write", or several joined by anything: "read,write" and
// "read+write" both work, because both get typed.
//
// A word nobody recognises used to leave the flag set empty, which then fell
// back to the default - so `access: "wrtie"` armed a fetch breakpoint, reported
// success, echoed the typo back, and fired somewhere the caller was not
// watching. The scope argument two lines below had been validated all along.
static unsigned char accessFlags(const std::string& text, unsigned char def, bool allowExec) {
	unsigned char flags = 0;
	std::string rest = text;
	for (char& ch : rest) ch = (char)tolower((unsigned char)ch);
	size_t recognised = 0;
	struct Word { const char* name; unsigned char bit; bool exec; };
	static const Word words[] = {
		{"exec", MEM_BRK_FETCH, true},
		{"read", MEM_BRK_RD, false},
		{"write", MEM_BRK_WR, false},
	};
	for (const Word& w : words) {
		size_t at = rest.find(w.name);
		while (at != std::string::npos) {
			if (!w.exec || allowExec) flags |= w.bit;
			else throw std::runtime_error("access: a port breakpoint has no 'exec' - "
						      "the CPU never executes out of a port");
			recognised += strlen(w.name);
			at = rest.find(w.name, at + 1);
			break;
		}
	}
	// Anything left over that is not a separator is a word we did not
	// understand, and understanding half of an argument is not understanding it.
	size_t letters = 0;
	for (char ch : rest) if (isalpha((unsigned char)ch)) letters++;
	if (!flags || letters != recognised)
		throw std::runtime_error("access: '" + text + "' is not one of exec, read, write "
					 "(or several of them, as in \"read,write\")");
	(void)def;
	return flags;
}

void registerDebugTools() {
	tool("set_breakpoint",
	     "Break when the CPU touches an address. access: exec (default), read, write, or any "
	     "combination like \"read,write\". scope: \"cell\" (default) puts the breakpoint in the "
	     "memory cell, so it follows the bank; \"address\" puts it on the CPU address, so it "
	     "fires whatever is paged in there. A banked label (\"05:200E main_loop\") or an explicit "
	     "\"bank:offset\" arms that bank's cell even when the bank is not paged in right now - "
	     "without one, cell scope can only mean whatever is mapped at the time.",
	     json{{"properties", {
		     {"address", {{"type", "string"}, {"description", kAddrArg}}},
		     {"access", {{"type", "string"}, {"description", "exec | read | write | \"read,write\""}}},
		     {"scope", {{"type", "string"}, {"description", "cell (default) | address"}}}
	     }}, {"required", json::array({"address"})}},
	     [](const json& a) {
		     int adr = 0, bank = -1;
		     if (!argAddrBank(a, "address", adr, bank)) throw std::runtime_error("address required");
		     const std::string acc = argStr(a, "access", "exec");
		     const unsigned char flags = accessFlags(acc, MEM_BRK_FETCH, true);

		     std::string scope = argStr(a, "scope", "cell");
		     if (scope != "cell" && scope != "address")
			     throw std::runtime_error("scope must be \"cell\" or \"address\", not '" + scope + "'");

		     json out{{"address", adr}, {"address_hex", hex16(adr)},
			     {"access", acc}, {"flags", flags}, {"scope", scope}, {"ok", true}};
		     if (scope == "address") {
			     mach().setBreakAddress(adr, flags);
		     } else if (bank >= 0) {
			     mach().setBreakBank(bank, adr, flags);
			     out["bank"] = bank;
			     out["bank_paged_in"] = (mach().bankAt(adr) == bank);
		     } else {
			     mach().setBreak(adr, flags);
			     out["bank"] = mach().bankAt(adr);
			     out["phys_address"] = mach().physAddress(adr);
		     }
		     return out;
	     });

	tool("clear_breakpoints",
	     "Remove every breakpoint set through this server (memory, address and I/O).",
	     json{},
	     [](const json&) { mach().clearBreaks(); return json{{"ok", true}}; });

	tool("set_port_breakpoint",
	     "Break on an I/O port access. access: read, write or both. This is how paging and "
	     "border writes are caught without knowing where in the code they happen: $7FFD for "
	     "the 128K pager, $FE for border and beeper.",
	     json{{"properties", {
		     {"port", {{"type", "string"}, {"description", "port number, or a label/EQU name"}}},
		     {"access", {{"type", "string"}}}
	     }}, {"required", json::array({"port"})}},
	     [](const json& a) {
		     // A Z80 port is 16 bits like an address, and a number outside that
		     // reaches the core as a wrapped one.
		     const int port = argAddrRequired(a, "port");
		     const std::string acc = argStr(a, "access", "read,write");
		     // A port has no fetch: the CPU never executes out of one.
		     const unsigned char flags = accessFlags(acc, MEM_BRK_RD | MEM_BRK_WR, false);
		     mach().setIoBreak(port, flags);
		     return json{{"port", port}, {"access", acc}, {"ok", true}};
	     });

	// -------------------------------------------------- vision

	tool("trace",
	     "Ring buffer of executed addresses - the backtrace for \"where did the CPU go?\". "
	     "action: enable, disable, clear, dump. dump disassembles the last `count` addresses.",
	     json{{"properties", {
		     {"action", {{"type", "string"}}},
		     {"count", {{"type", "integer"}, {"description", "entries to dump, default 32"}}},
		     {"size", {{"type", "integer"}, {"description", "ring size when enabling, default 4096"}}}
	     }}},
	     [](const json& a) {
		     auto& t = mach().trace();
		     std::string action = argStr(a, "action", "dump");
		     if (action == "enable") {
			     t.reset((size_t)argNumRange(a, "size", 4096, 1, 1 << 20));
			     t.on = true;
		     } else if (action == "disable") {
			     t.on = false;
		     } else if (action == "clear") {
			     t.reset(t.capacity);
		     } else if (action != "dump") {
			     throw std::runtime_error("action must be enable, disable, clear or dump");
		     }
		     json lines = json::array();
		     if (action == "dump") {
			     for (int pc : t.dump((size_t)argNumRange(a, "count", 32, 1, 4096))) {
				     int len = 0;
				     xMnem mn;
				     json e{{"address", pc}, {"address_hex", hex16(pc)},
					    {"text", mach().disasm(pc, &len, &mn)}};
				     if (const std::string* lab = mach().labels().atAddress(pc))
					     e["label"] = *lab;
				     lines.push_back(e);
			     }
		     }
		     return json{{"running", t.on}, {"capacity", (int)t.capacity}, {"trace", lines}};
	     });
}

} // namespace tools
} // namespace xsp
