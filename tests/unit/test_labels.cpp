#include "unit.h"
#include "xsp_labels.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace xsp;

namespace {

std::string writeTemp(const char* stem, const std::string& body) {
	std::string path = std::string(P_tmpdir) + "/xsp-unit-" + stem;
	std::ofstream f(path, std::ios::binary);
	f << body;
	f.close();
	return path;
}

} // namespace

void test_labels() {
	unit::begin("labels");

	// ---- bank:offset to a CPU address -------------------------------------
	//
	// This is the 128K memory map and nothing else: bank 5 is wired at $4000,
	// bank 2 at $8000, and every other bank is only ever visible through the
	// window at $C000. Getting it wrong puts a breakpoint in the wrong bank,
	// which looks exactly like a breakpoint that never fires.
	CHECK_EQ(Labels::mapBankOffset(5, 0x0100), 0x4100);
	CHECK_EQ(Labels::mapBankOffset(2, 0x0100), 0x8100);
	CHECK_EQ(Labels::mapBankOffset(0, 0x0100), 0xC100);
	CHECK_EQ(Labels::mapBankOffset(1, 0x0100), 0xC100);
	CHECK_EQ(Labels::mapBankOffset(3, 0x0100), 0xC100);
	CHECK_EQ(Labels::mapBankOffset(7, 0x0001), 0xC001);

	// $FF is the marker for "this is already a CPU address", so it passes
	// through with no window arithmetic at all.
	CHECK_EQ(Labels::mapBankOffset(0xff, 0x8000), 0x8000);
	CHECK_EQ(Labels::mapBankOffset(0xff, 0x0000), 0x0000);
	CHECK_EQ(Labels::mapBankOffset(0xff, 0xFFFF), 0xFFFF);

	// An offset is 14 bits. A bank-relative value that overflows one is a
	// malformed symbol file, and it must fold into the window rather than
	// running off the end of it.
	CHECK_EQ(Labels::mapBankOffset(5, 0x4000), 0x4000);
	CHECK_EQ(Labels::mapBankOffset(5, 0x3FFF), 0x7FFF);
	CHECK_EQ(Labels::mapBankOffset(7, 0x8123), 0xC123);

	// ---- reading a symbol file --------------------------------------------
	{
		const std::string path = writeTemp("labels.txt",
			"FF:8000 main\n"
			"FF:8010 draw_loop\n"
			"sprite_data: EQU $9000\n"
			"05:0100 bank5_label\n"
			"02:1000 bank2_main\n"
			"07:0001 bank7_const\n"
			":8020 colon_only\n"
			"hl: EQU 5\n"
			"\n"
			"; a comment line\n"
			"FF:8000 second_name_same_address\n");

		Labels lab;
		std::string err;
		CHECK(lab.load(path, err));
		CHECK(err.empty());

		int adr = -1;
		CHECK(lab.find("main", adr));      CHECK_EQ(adr, 0x8000);
		CHECK(lab.find("draw_loop", adr)); CHECK_EQ(adr, 0x8010);
		CHECK(lab.find("bank5_label", adr)); CHECK_EQ(adr, 0x4100);
		CHECK(lab.find("bank2_main", adr));  CHECK_EQ(adr, 0x9000);
		CHECK(lab.find("bank7_const", adr)); CHECK_EQ(adr, 0xC001);

		// An empty bank field means the same as FF.
		CHECK(lab.find("colon_only", adr)); CHECK_EQ(adr, 0x8020);

		// EQU constants are indistinguishable from addresses in this format,
		// which is the format's problem and not something to paper over: they
		// have to load, and they have to keep their bank as "not a bank".
		CHECK(lab.find("sprite_data", adr)); CHECK_EQ(adr, 0x9000);
		const Labels::Entry* e = lab.entry("sprite_data");
		CHECK(e != nullptr);
		if (e) CHECK_EQ(e->bank, -1);
		e = lab.entry("bank5_label");
		CHECK(e != nullptr);
		if (e) CHECK_EQ(e->bank, 5);

		// "hl: EQU 5" starts with something that looks like a bank field.
		// It must be read as a constant named hl, not as bank $HL.
		CHECK(lab.find("hl", adr)); CHECK_EQ(adr, 5);

		CHECK(!lab.find("no_such_label", adr));

		// Two names on one address: the reverse lookup keeps the first, and
		// both still resolve forward. Anything else makes disassembly flip
		// between names depending on file order.
		const std::string* nm = lab.atAddress(0x8000);
		CHECK(nm != nullptr);
		if (nm) CHECK_EQ(*nm, std::string("main"));
		CHECK(lab.find("second_name_same_address", adr));
		CHECK_EQ(adr, 0x8000);

		CHECK(lab.atAddress(0x1234) == nullptr);
		CHECK_EQ(lab.source(), path);

		// `substitute` is deliberately blind to what a word means: it
		// replaces every name it knows, including one that happens to be
		// spelled like a register. That is not an oversight to test around.
		// The guard lives one layer up, in isReservedAsmName() in
		// xsp_machine.cpp, which refuses to define such a label in the
		// first place - because "c: ... jr c,x" assembles cleanly as
		// something else entirely, and a clean wrong answer is worse than
		// an error. Pinning the split here means a later attempt to "fix"
		// it in the wrong layer fails a test that says why.
		CHECK_EQ(lab.substitute("ld hl,main"), std::string("ld 5,32768"));

		lab.clear();
		CHECK_EQ((int)lab.size(), 0);
		CHECK(!lab.find("main", adr));

		std::remove(path.c_str());
	}

	// ---- substitution into operand text -----------------------------------
	//
	// Names become decimal so the result can be handed to an assembler. What
	// must not happen is a partial match eating part of a longer identifier,
	// or a bare number being treated as a name.
	{
		const std::string path = writeTemp("subst.txt",
			"FF:8000 main\n"
			"FF:8010 draw_loop\n");
		Labels lab;
		std::string err;
		CHECK(lab.load(path, err));

		CHECK_EQ(lab.substitute("ld hl,main"), std::string("ld hl,32768"));
		CHECK_EQ(lab.substitute("jp draw_loop"), std::string("jp 32784"));
		CHECK_EQ(lab.substitute("ld a,(unknown_name)"),
			 std::string("ld a,(unknown_name)"));
		CHECK_EQ(lab.substitute("ld a,16"), std::string("ld a,16"));
		// `mainly` starts with `main` and is a different word
		CHECK_EQ(lab.substitute("call mainly"), std::string("call mainly"));
		// and so does a name that ends with one
		CHECK_EQ(lab.substitute("call submain"), std::string("call submain"));
		CHECK_EQ(lab.substitute(""), std::string(""));
		// every occurrence, not just the first
		CHECK_EQ(lab.substitute("main main"), std::string("32768 32768"));

		lab.clear();
		// With nothing loaded, substitution is the identity.
		CHECK_EQ(lab.substitute("ld hl,main"), std::string("ld hl,main"));

		std::remove(path.c_str());
	}

	// ---- files that are not symbol files ---------------------------------
	{
		Labels lab;
		std::string err;
		CHECK(!lab.load("/nonexistent/xsp-unit/none.txt", err));
		CHECK(!err.empty());

		const std::string junk = writeTemp("junk.txt", "hello world\n1234\n");
		err.clear();
		CHECK(!lab.load(junk, err));
		CHECK(!err.empty());		// says so rather than loading nothing quietly
		std::remove(junk.c_str());
	}
}
