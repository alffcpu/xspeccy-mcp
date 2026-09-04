// Reading the arguments of a tool call. These were unreachable from a test
// until they had a file of their own, and they are worth reaching: the caller
// on the other end is a language model generating JSON, so a wrong argument is
// the normal case rather than the exceptional one, and every one of these
// functions decides between refusing it and quietly acting on something else.
#include "unit.h"
#include "xsp_args.h"
#include "xsp_blend.h"

#include <stdexcept>
#include <string>

using namespace xsp;
using namespace xsp::args;

namespace {

// A check that something is refused, and refused with a message that says what
// was wrong. An exception with an empty or generic message is not much better
// than a wrong answer, because the caller cannot correct it.
#define CHECK_THROWS_MENTIONING(expr, needle)                                  \
	do {                                                                   \
		++::unit::g_checks;                                            \
		bool _threw = false;                                           \
		std::string _what;                                             \
		try { (void)(expr); }                                          \
		catch (const std::exception& e) { _threw = true; _what = e.what(); } \
		if (!_threw)                                                   \
			::unit::fail(#expr " throws", __FILE__, __LINE__,      \
				     "it did not");                            \
		else if (_what.find(needle) == std::string::npos)              \
			::unit::fail(#expr " explains itself", __FILE__, __LINE__, \
				     "message was \"" + _what +                \
				     "\", expected it to mention " + (needle)); \
	} while (0)

} // namespace

void test_args() {
	unit::begin("args");

	// ---- numbers ------------------------------------------------------------
	{
		int v = -1;
		CHECK(argNum(json{{"n", 42}}, "n", v));
		CHECK_EQ(v, 42);
		CHECK(argNum(json{{"n", -7}}, "n", v));
		CHECK_EQ(v, -7);

		// A float is truncated rather than refused: a caller that computed an
		// address arithmetically may well hand over 16384.0.
		CHECK(argNum(json{{"n", 16384.0}}, "n", v));
		CHECK_EQ(v, 16384);

		// Every notation a ZX programmer writes an address in.
		const struct { const char* text; int want; } forms[] = {
			{"1234", 1234}, {"$4000", 0x4000}, {"0x4000", 0x4000},
			{"#4000", 0x4000}, {"0X8000", 0x8000}, {"$ff", 0xff},
		};
		for (const auto& f : forms) {
			CHECK(argNum(json{{"n", f.text}}, "n", v));
			CHECK_EQ(v, f.want);
		}

		// Absent, null, or not an object at all: not an error, just no value,
		// so the caller's default stands.
		v = 99;
		CHECK(!argNum(json::object(), "n", v));
		CHECK_EQ(v, 99);
		CHECK(!argNum(json{{"n", nullptr}}, "n", v));
		CHECK(!argNum(json::array(), "n", v));
		CHECK(!argNum(json(5), "n", v));

		// Present and unreadable is the case that matters: a typo must not
		// become a zero.
		CHECK_THROWS_MENTIONING(argNum(json{{"n", "abc"}}, "n", v), "not a number");
		CHECK_THROWS_MENTIONING(argNum(json{{"n", true}}, "n", v), "not a number");
		CHECK_THROWS_MENTIONING(argNum(json{{"n", json::array({1})}}, "n", v), "not a number");
		// The message names the argument, because a call with eight of them
		// needs to say which one.
		CHECK_THROWS_MENTIONING(argNum(json{{"lines", "abc"}}, "lines", v), "lines");
	}

	// argNumOr is the same with the default folded in.
	{
		CHECK_EQ(argNumOr(json{{"n", 3}}, "n", 9), 3);
		CHECK_EQ(argNumOr(json::object(), "n", 9), 9);
		CHECK_THROWS_MENTIONING(argNumOr(json{{"n", "x"}}, "n", 9), "not a number");
	}

	// ---- ranges -------------------------------------------------------------
	//
	// Refused, never clamped. A value the caller cannot have meant is a mistake
	// worth reporting, and moving it quietly answers a different question.
	{
		CHECK_EQ(argNumRange(json{{"n", 5}}, "n", 1, 1, 10), 5);
		CHECK_EQ(argNumRange(json{{"n", 1}}, "n", 1, 1, 10), 1);	// inclusive
		CHECK_EQ(argNumRange(json{{"n", 10}}, "n", 1, 1, 10), 10);
		CHECK_THROWS_MENTIONING(argNumRange(json{{"n", 11}}, "n", 1, 1, 10), "outside");
		CHECK_THROWS_MENTIONING(argNumRange(json{{"n", 0}}, "n", 1, 1, 10), "1..10");

		// The default is ours and is trusted even when it is outside the range
		// the caller would be held to: -1 means "unset" for several tools.
		CHECK_EQ(argNumRange(json::object(), "n", -1, 0, 10), -1);
	}

	// ---- addresses ----------------------------------------------------------
	{
		int v = 0;
		CHECK(argAddr(json{{"address", 0x4000}}, "address", v));
		CHECK_EQ(v, 0x4000);
		CHECK(argAddr(json{{"address", "$5B00"}}, "address", v));
		CHECK_EQ(v, 0x5b00);
		CHECK(!argAddr(json::object(), "address", v));

		// Neither a number nor a name.
		CHECK_THROWS_MENTIONING(argAddr(json{{"address", true}}, "address", v),
					"neither a number nor a label");

		// A name that is not a number is looked up in the symbol table, and
		// with none loaded the message has to say so rather than just "no such
		// label": the caller's next move is load_labels, not a different name.
		CHECK_THROWS_MENTIONING(argAddr(json{{"address", "main_loop"}}, "address", v),
					"load_labels");

		// The address space is 64K and nothing outside it is an address.
		CHECK_EQ(argAddrRange(json{{"address", 0}}, "address", 0), 0);
		CHECK_EQ(argAddrRange(json{{"address", 0xffff}}, "address", 0), 0xffff);
		CHECK_EQ(argAddrRange(json::object(), "address", 0x1234), 0x1234);
		CHECK_THROWS_MENTIONING(argAddrRange(json{{"address", 0x10000}}, "address", 0),
					"$0000..$FFFF");
		CHECK_THROWS_MENTIONING(argAddrRange(json{{"address", -1}}, "address", 0),
					"address space");
	}

	// "bank:offset", both parts hex, as the symbol file spells it. bank stays
	// -1 for a plain address, which means "wherever it is mapped now" - a
	// different question from "in page 5", and the two must not be confused.
	{
		int v = 0, bank = 0;
		CHECK(argAddrBank(json{{"address", "05:0000"}}, "address", v, bank));
		CHECK_EQ(bank, 5);
		CHECK_EQ(v, Labels::mapBankOffset(5, 0));

		CHECK(argAddrBank(json{{"address", "02:1234"}}, "address", v, bank));
		CHECK_EQ(bank, 2);
		CHECK_EQ(v, Labels::mapBankOffset(2, 0x1234));

		CHECK(argAddrBank(json{{"address", 0x8000}}, "address", v, bank));
		CHECK_EQ(bank, -1);
		CHECK_EQ(v, 0x8000);

		CHECK(argAddrBank(json{{"address", "$C000"}}, "address", v, bank));
		CHECK_EQ(bank, -1);
		CHECK_EQ(v, 0xc000);
	}

	// ---- strings and flags --------------------------------------------------
	//
	// Both fall back to the default rather than throwing, because both are
	// asking "did you say anything about this", and a number where a string
	// belongs is answered by the tool's own schema.
	{
		CHECK_EQ(argStr(json{{"s", "hello"}}, "s"), std::string("hello"));
		CHECK_EQ(argStr(json::object(), "s", "fallback"), std::string("fallback"));
		CHECK_EQ(argStr(json{{"s", 5}}, "s", "fallback"), std::string("fallback"));
		CHECK_EQ(argStr(json{{"s", ""}}, "s", "fallback"), std::string(""));

		CHECK(argBool(json{{"b", true}}, "b", false));
		CHECK(!argBool(json{{"b", false}}, "b", true));
		CHECK(argBool(json::object(), "b", true));
		CHECK(argBool(json{{"b", "yes"}}, "b", true));	// not a boolean: default
		CHECK(argBool(json{{"b", 1}}, "b", true));
	}

	// ---- doubles ------------------------------------------------------------
	{
		double d = 0;
		CHECK(argDouble(json{{"g", 2.4}}, "g", d));
		CHECK(d > 2.39 && d < 2.41);
		CHECK(argDouble(json{{"g", 2}}, "g", d));
		CHECK_EQ(d, 2.0);
		CHECK(argDouble(json{{"g", "2.2"}}, "g", d));
		CHECK(d > 2.19 && d < 2.21);
		CHECK(!argDouble(json::object(), "g", d));
		CHECK(!argDouble(json{{"g", nullptr}}, "g", d));
		CHECK_THROWS_MENTIONING(argDouble(json{{"g", "wide"}}, "g", d), "not a number");
		CHECK_THROWS_MENTIONING(argDouble(json{{"g", true}}, "g", d), "not a number");
	}

	// ---- how many frames a picture is made of --------------------------------
	//
	// -1 means "whatever video_config last set", which is why absent and 0 are
	// different answers rather than both being falsy.
	{
		CHECK_EQ(argBlend(json::object()), -1);
		CHECK_EQ(argBlend(json{{"blend", nullptr}}), -1);
		CHECK_EQ(argBlend(json{{"blend", true}}), 2);	// a gigascreen
		CHECK_EQ(argBlend(json{{"blend", false}}), 1);	// the raw frame
		CHECK_EQ(argBlend(json{{"blend", 3}}), 3);
		CHECK_EQ(argBlend(json{{"blend", 0}}), 0);
		CHECK_THROWS_MENTIONING(argBlend(json{{"blend", blend::kMaxHistory + 1}}), "outside");
	}

	// What the blend did, and the notes that stop a caller reading a duplicate
	// as a broken effect.
	{
		xsp::video::BlendInfo info;
		info.on = true;
		info.frames = 2; info.used = 2; info.distinct = 2;
		info.gamma = 2.2; info.pattern = "flicker";
		json j = blendJson(info);
		CHECK_EQ(j.value("frames", 0), 2);
		CHECK_EQ(j.value("distinct_frames", 0), 2);
		CHECK_EQ(j.value("pattern", ""), std::string("flicker"));
		CHECK(!j.contains("note"));		// nothing to warn about

		// Not enough history yet: the picture is not what was asked for.
		info.used = 1;
		CHECK(blendJson(info).value("note", "").find("run more") != std::string::npos);

		// Enough frames, but they are the same picture twice.
		info.used = 2; info.distinct = 1;
		CHECK(blendJson(info).value("note", "").find("duplicates") != std::string::npos);

		// All different, but because the effect is animating rather than
		// alternating: blending those is motion blur, which is a different
		// complaint from the one above and needs saying separately.
		info.distinct = 2; info.pattern = "animation";
		CHECK(blendJson(info).value("note", "").find("motion blur") != std::string::npos);
	}

	// ---- hex ----------------------------------------------------------------
	//
	// strtol used to do this, and it took "GG" for zero and "1234" for a byte,
	// so a typo turned into a search for something else that then found it.
	{
		CHECK_EQ((int)parseHexByte("00", "bytes"), 0);
		CHECK_EQ((int)parseHexByte("ff", "bytes"), 255);
		CHECK_EQ((int)parseHexByte("FF", "bytes"), 255);
		CHECK_EQ((int)parseHexByte("3e", "bytes"), 0x3e);
		CHECK_EQ((int)parseHexByte("3E", "bytes"), 0x3e);
		CHECK_EQ((int)parseHexByte("5", "bytes"), 5);		// one digit is a byte

		CHECK_THROWS_MENTIONING(parseHexByte("", "bytes"), "hex byte");
		CHECK_THROWS_MENTIONING(parseHexByte("GG", "bytes"), "GG");
		CHECK_THROWS_MENTIONING(parseHexByte("1234", "bytes"), "1234");
		CHECK_THROWS_MENTIONING(parseHexByte("0x3e", "bytes"), "hex byte");
		CHECK_THROWS_MENTIONING(parseHexByte("-1", "bytes"), "hex byte");
	}

	{
		CHECK_EQ(hex16(0), std::string("$0000"));
		CHECK_EQ(hex16(0x4000), std::string("$4000"));
		CHECK_EQ(hex16(0xffff), std::string("$FFFF"));
		// Always four digits and always in range, so that a number that has
		// wandered outside the address space still prints as an address.
		CHECK_EQ(hex16(0x10000), std::string("$0000"));
		CHECK_EQ(hex16(-1), std::string("$FFFF"));
	}

	// ---- where a frame ends --------------------------------------------------
	//
	// The default is a HALT and not an interrupt, because an effect frame is
	// the code between two HALTs and a render that overruns spans several
	// interrupts. Measuring those as separate frames is how a slow effect
	// reports as a fast one.
	{
		SyncSpec fallback = argSync(json::object());
		CHECK(!fallback.byInterrupt);
		CHECK_EQ(fallback.pc, -1);
		CHECK_EQ(fallback.text, std::string("halt"));

		CHECK(!argSync(json{{"sync", "halt"}}).byInterrupt);

		SyncSpec byFrame = argSync(json{{"sync", "frame"}});
		CHECK(byFrame.byInterrupt);
		CHECK_EQ(byFrame.text, std::string("frame"));
		// Two spellings of the same request, because both are what people say.
		CHECK(argSync(json{{"sync", "interrupt"}}).byInterrupt);

		SyncSpec byAddress = argSync(json{{"sync", "$8000"}});
		CHECK(!byAddress.byInterrupt);
		CHECK_EQ(byAddress.pc, 0x8000);
		CHECK_EQ(byAddress.text, std::string("$8000"));

		// A different key, because two tools take two syncs in one call.
		CHECK(argSync(json{{"until", "frame"}}, "until").byInterrupt);
		CHECK(!argSync(json{{"sync", "frame"}}, "until").byInterrupt);
	}
}
