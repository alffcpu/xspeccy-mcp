#include "unit.h"
#include "xsp_listing.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace xsp;

void test_listing() {
	unit::begin("listing");

	// An sjasmplus listing: line number, address, the bytes, then the source.
	// The byte count per line is what makes "which line is this address in"
	// answerable, so the fixture deliberately mixes 1, 2 and 3 byte opcodes.
	const std::string path = std::string(P_tmpdir) + "/xsp-unit-listing.lst";
	{
		std::ofstream f(path, std::ios::binary);
		f << "# file main.asm\n"
		     "     1   0000              ; a comment, no bytes\n"
		     "     2   8000              main:\n"
		     "     3   8000 F3           di\n"
		     "     4   8001 3E 10        ld a,16\n"
		     "     5   8003 EE 10        xor 16\n"
		     "     6   8005 D3 FE        out (254),a\n"
		     "     7   8007 21 00 40     ld hl,$4000\n"
		     "     8   800A C9           ret\n";
	}

	Listing lst;
	std::string err;
	CHECK(lst.load(path, err));
	CHECK(err.empty());
	CHECK(lst.size() > 0);
	CHECK_EQ(lst.source(), path);

	// ---- exact match versus covering match --------------------------------
	//
	// These are different questions and the difference is the whole reason
	// both exist. $8001 is where `ld a,16` starts; $8002 is its second byte
	// and belongs to the same source line while being no line's address.
	{
		const SourceLine* exact = lst.atAddress(0x8001);
		CHECK(exact != nullptr);
		if (exact) CHECK_EQ(exact->line, 4);

		CHECK(lst.atAddress(0x8002) == nullptr);

		const SourceLine* cover = lst.coveringAddress(0x8002);
		CHECK(cover != nullptr);
		if (cover) {
			CHECK_EQ(cover->line, 4);
			CHECK_EQ(cover->address, 0x8001);
			CHECK_EQ(cover->bytes, 2);
		}
	}

	// The three-byte instruction, whose middle and last bytes must both land
	// on it. An off-by-one here reports the next instruction instead, which
	// during a step looks like the program counter jumping a line early.
	{
		const SourceLine* a = lst.coveringAddress(0x8008);
		const SourceLine* b = lst.coveringAddress(0x8009);
		CHECK(a != nullptr && b != nullptr);
		if (a) CHECK_EQ(a->line, 7);
		if (b) CHECK_EQ(b->line, 7);
		// and the byte after it is the next line, not still this one
		const SourceLine* c = lst.coveringAddress(0x800A);
		CHECK(c != nullptr);
		if (c) CHECK_EQ(c->line, 8);
	}

	// Outside the assembled range there is no line, and saying so beats
	// returning the nearest one.
	CHECK(lst.coveringAddress(0x9000) == nullptr);
	CHECK(lst.coveringAddress(0x0000) == nullptr);

	// A line that assembled to no bytes covers no address at all. $8000 is
	// the label line and the `di` line both; the byte belongs to `di`.
	{
		const SourceLine* s = lst.coveringAddress(0x8000);
		CHECK(s != nullptr);
		if (s) CHECK_EQ(s->bytes, 1);
	}

	// ---- line number to address -------------------------------------------
	{
		int adr = -1;
		CHECK(lst.addressOfLine(3, adr));
		CHECK_EQ(adr, 0x8000);
		CHECK(lst.addressOfLine(8, adr));
		CHECK_EQ(adr, 0x800A);
		CHECK(!lst.addressOfLine(999, adr));
	}

	// ---- a window of source around an address -----------------------------
	{
		std::vector<SourceLine> w = lst.around(0x8003, 1, 1);
		CHECK_EQ((int)w.size(), 3);
		if (w.size() == 3) {
			CHECK_EQ(w[0].line, 4);
			CHECK_EQ(w[1].line, 5);
			CHECK_EQ(w[2].line, 6);
		}

		// Asking for more context than exists must clamp at both ends
		// rather than read past them.
		std::vector<SourceLine> big = lst.around(0x8000, 100, 100);
		CHECK(!big.empty());
		CHECK(big.size() <= lst.size() + 4);

		// No covering line means no window.
		CHECK(lst.around(0x9000, 2, 2).empty());
	}

	lst.clear();
	CHECK_EQ((int)lst.size(), 0);
	CHECK(lst.coveringAddress(0x8000) == nullptr);

	std::remove(path.c_str());

	// A file that is not a listing must be refused rather than loaded empty.
	{
		Listing l2;
		std::string e2;
		CHECK(!l2.load("/nonexistent/xsp-unit/none.lst", e2));
		CHECK(!e2.empty());
	}
}
