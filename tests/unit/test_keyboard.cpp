// The ZX keyboard is a 8x5 matrix of half-rows, and every one of the three
// things this module does can be wrong in a way that still looks plausible from
// the outside: a key on the wrong half-row types a different letter, a missing
// shift types the unshifted character, and a name the tool advertises but does
// not accept fails only for the one caller who tries it.
#include "unit.h"
#include "xsp_keyboard.h"

#include <cstring>
#include <memory>
#include <set>
#include <string>

using namespace xsp;
using xsp::keyboard::Key;

namespace {

// A machine with nothing in it but a keyboard. press()/release() touch
// comp->keyb->map and nothing else, so this is the whole world they need, and
// building it here means the matrix can be tested without an emulator.
//
// On the heap, not the stack: Computer carries the machine's whole memory
// inline and is over four megabytes, which overflows a default stack the moment
// two of these are alive at once.
class FakeMachine {
public:
	FakeMachine()
		: m_comp(new Computer), m_keyb(new Keyboard) {
		std::memset(m_comp.get(), 0, sizeof(Computer));
		std::memset(m_keyb.get(), 0, sizeof(Keyboard));
		m_comp->keyb = m_keyb.get();
		keyboard::releaseAll(m_comp.get());
	}

	Computer* comp() { return m_comp.get(); }

	// A half-row as the ULA would read it: a set bit is a key that is up.
	int row(int r) const { return m_keyb->map[r] & 0xff; }

	bool allUp() const {
		for (int i = 0; i < 8; i++)
			if ((m_keyb->map[i] & 0xff) != 0xff) return false;
		return true;
	}

private:
	std::unique_ptr<Computer> m_comp;
	std::unique_ptr<Keyboard> m_keyb;
};

Key of(const char* name) {
	Key k;
	CHECK(keyboard::lookup(name, k));
	return k;
}

} // namespace

void test_keyboard() {
	unit::begin("keyboard");

	// ---- the matrix itself ------------------------------------------------
	//
	// Forty keys, each on its own (row, mask). A duplicated cell would make two
	// letters type the same thing, which is the one bug in here that no
	// higher-level test can see: the wrong letter still arrives as a letter.
	{
		std::set<int> seen;
		const char* letters = "abcdefghijklmnopqrstuvwxyz0123456789";
		for (const char* p = letters; *p; p++) {
			Key k;
			CHECK(keyboard::forChar(*p, k));
			CHECK(k.row >= 0 && k.row < 8);
			CHECK(k.mask == 1 || k.mask == 2 || k.mask == 4 ||
			      k.mask == 8 || k.mask == 16);
			CHECK(!k.caps && !k.symbol);
			seen.insert(k.row * 32 + k.mask);
		}
		CHECK_EQ((int)seen.size(), 36);

		// enter and space are the remaining two unshifted keys, and they are
		// not letters, so they are counted separately rather than not at all.
		Key enter = of("enter"), space = of("space");
		seen.insert(enter.row * 32 + enter.mask);
		seen.insert(space.row * 32 + space.mask);
		CHECK_EQ((int)seen.size(), 38);
	}

	// A handful of cells against libxpeccy's own keyTab, so that a wholesale
	// renumbering of the matrix fails here rather than in a screenshot. Row 0 is
	// the half-row selected by A15, which is the one carrying space.
	{
		Key a = of("a"), q = of("q"), one = of("1"), zero = of("0");
		CHECK_EQ(a.row, 6);   CHECK_EQ(a.mask, 1);
		CHECK_EQ(q.row, 5);   CHECK_EQ(q.mask, 1);
		CHECK_EQ(one.row, 4); CHECK_EQ(one.mask, 1);
		CHECK_EQ(zero.row, 3); CHECK_EQ(zero.mask, 1);
		Key space = of("space");
		CHECK_EQ(space.row, 0); CHECK_EQ(space.mask, 1);
	}

	// ---- names ------------------------------------------------------------
	//
	// Every name the tool advertises has to be a name the tool accepts. These
	// are two different lists in the source and nothing else compares them.
	{
		std::vector<std::string> all = keyboard::names();
		CHECK(all.size() >= 20);
		for (const std::string& name : all) {
			Key k;
			CHECK(keyboard::lookup(name, k));
			CHECK(k.row >= 0);
		}
	}

	// Case does not matter for a name, and a name that does not exist is a
	// failure rather than a silently ignored keypress.
	{
		Key lower, upper, mixed, missing;
		CHECK(keyboard::lookup("enter", lower));
		CHECK(keyboard::lookup("ENTER", upper));
		CHECK(keyboard::lookup("EnTeR", mixed));
		CHECK_EQ(lower.row, upper.row);
		CHECK_EQ(lower.mask, mixed.mask);
		CHECK(!keyboard::lookup("f1", missing));
		CHECK(!keyboard::lookup("", missing));
		CHECK(!keyboard::lookup("escape", missing));
	}

	// ---- shifts -----------------------------------------------------------
	//
	// An upper-case letter is the same cell as the lower-case one plus Caps.
	{
		Key lower, upper;
		CHECK(keyboard::forChar('g', lower));
		CHECK(keyboard::forChar('G', upper));
		CHECK_EQ(upper.row, lower.row);
		CHECK_EQ(upper.mask, lower.mask);
		CHECK(!lower.caps);
		CHECK(upper.caps);
		CHECK(!upper.symbol);
	}

	// The punctuation the ROM puts on Symbol Shift. Each has to land on the
	// key that actually carries it, which is the fact worth writing down: '-'
	// is on J and ',' is on N, and neither is guessable from the character.
	{
		struct { char ch; const char* base; } pairs[] = {
			{'-', "j"}, {'+', "k"}, {'=', "l"}, {':', "z"}, {';', "o"},
			{'"', "p"}, {',', "n"}, {'.', "m"}, {'/', "v"}, {'*', "b"},
			{'<', "r"}, {'>', "t"}, {'?', "c"}, {'[', "y"}, {']', "u"},
			{'@', "2"}, {'$', "4"}, {'(', "8"}, {')', "9"}, {'_', "0"},
		};
		for (const auto& p : pairs) {
			Key sym, base;
			CHECK(keyboard::forChar(p.ch, sym));
			CHECK(keyboard::forChar(p.base[0], base));
			CHECK_EQ(sym.row, base.row);
			CHECK_EQ(sym.mask, base.mask);
			CHECK(sym.symbol);
			CHECK(!sym.caps);
		}
	}

	// '^' and '!' name the two shift keys inside the matrix table, but as
	// characters they are ordinary Symbol Shift punctuation: '^' is on H and
	// '!' is on 1. Typing "!" must not press Symbol Shift on its own and type
	// nothing, which is what a table lookup that stopped one line earlier would
	// do. The words "caps" and "symbol" are how the shift keys are asked for.
	{
		Key caret, bang, caps, symbol, h, one;
		CHECK(keyboard::forChar('^', caret));
		CHECK(keyboard::forChar('h', h));
		CHECK_EQ(caret.row, h.row);
		CHECK_EQ(caret.mask, h.mask);
		CHECK(caret.symbol);

		CHECK(keyboard::forChar('!', bang));
		CHECK(keyboard::forChar('1', one));
		CHECK_EQ(bang.row, one.row);
		CHECK_EQ(bang.mask, one.mask);
		CHECK(bang.symbol);

		CHECK(keyboard::lookup("caps", caps));
		CHECK(!caps.caps && !caps.symbol);
		CHECK(keyboard::lookup("symbol", symbol));
		CHECK(!symbol.caps && !symbol.symbol);
		CHECK(caps.row != symbol.row || caps.mask != symbol.mask);
	}

	// The editing keys are Caps Shift plus a digit, which is the only way a
	// 40-key machine has of expressing them.
	{
		Key del = of("delete"), left = of("left"), right = of("right");
		Key zero, five, eight;
		CHECK(keyboard::forChar('0', zero));
		CHECK(keyboard::forChar('5', five));
		CHECK(keyboard::forChar('8', eight));
		CHECK(del.caps);   CHECK_EQ(del.mask, zero.mask);   CHECK_EQ(del.row, zero.row);
		CHECK(left.caps);  CHECK_EQ(left.mask, five.mask);  CHECK_EQ(left.row, five.row);
		CHECK(right.caps); CHECK_EQ(right.mask, eight.mask); CHECK_EQ(right.row, eight.row);

		// "extend" is both shifts at once, which is neither of the above
		Key extend = of("extend");
		CHECK(extend.symbol);
	}

	// A carriage return is an enter, because a caller sending text from a file
	// with CRLF line endings means the same key as one sending "\n".
	{
		Key cr, lf;
		CHECK(keyboard::forChar('\r', cr));
		CHECK(keyboard::forChar('\n', lf));
		CHECK_EQ(cr.row, lf.row);
		CHECK_EQ(cr.mask, lf.mask);
	}

	// Characters a 40-key ZX has no way of producing are refused rather than
	// mapped to something near enough.
	{
		Key k;
		CHECK(!keyboard::forChar('\\', k));
		CHECK(!keyboard::forChar('{', k));
		CHECK(!keyboard::forChar('~', k));
		CHECK(!keyboard::forChar('|', k));
		CHECK(!keyboard::forChar('\t', k));
	}

	// ---- driving the matrix -----------------------------------------------
	//
	// A set bit is a key that is up. Pressing clears exactly the bits for the
	// key and its shifts, releasing puts back exactly those, and nothing else
	// in the eight half-rows moves.
	{
		FakeMachine m;
		CHECK(m.allUp());

		Key a = of("a");
		keyboard::press(m.comp(), a);
		CHECK_EQ(m.row(a.row), 0xff & ~a.mask);
		for (int r = 0; r < 8; r++)
			if (r != a.row) CHECK_EQ(m.row(r), 0xff);
		keyboard::release(m.comp(), a);
		CHECK(m.allUp());
	}

	// A shifted key holds down two cells at once, and lets go of both.
	{
		FakeMachine m;
		Key caps = of("caps");
		Key big;
		CHECK(keyboard::forChar('G', big));
		keyboard::press(m.comp(), big);
		CHECK_EQ(m.row(big.row), 0xff & ~big.mask);
		CHECK_EQ(m.row(caps.row), 0xff & ~caps.mask);
		keyboard::release(m.comp(), big);
		CHECK(m.allUp());
	}

	// releaseAll is what a tool calls when it does not know what is held, so it
	// has to clear a matrix in any state, including one nobody pressed.
	{
		FakeMachine m;
		keyboard::press(m.comp(), of("a"));
		keyboard::press(m.comp(), of("enter"));
		Key sym;
		CHECK(keyboard::forChar('*', sym));
		keyboard::press(m.comp(), sym);
		CHECK(!m.allUp());
		keyboard::releaseAll(m.comp());
		CHECK(m.allUp());
	}

	// A Key nobody filled in has row -1, and pressing it must do nothing at all
	// rather than write to map[-1].
	{
		FakeMachine m;
		Key empty;
		CHECK_EQ(empty.row, -1);
		keyboard::press(m.comp(), empty);
		CHECK(m.allUp());
		keyboard::release(m.comp(), empty);
		CHECK(m.allUp());
	}
}
