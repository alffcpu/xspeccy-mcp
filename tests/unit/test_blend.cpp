#include "unit.h"
#include "xsp_blend.h"

#include <cstring>
#include <vector>

using namespace xsp;

namespace {

// One RGBA pixel repeated: enough to exercise the arithmetic without a frame.
std::vector<unsigned char> solid(int w, int h, unsigned char r, unsigned char g,
				 unsigned char b, unsigned char a = 0xff) {
	std::vector<unsigned char> v((size_t)w * h * 4);
	for (size_t i = 0; i < v.size(); i += 4) {
		v[i] = r; v[i + 1] = g; v[i + 2] = b; v[i + 3] = a;
	}
	return v;
}

unsigned char mixTwoGrey(unsigned char a, unsigned char b, double gamma) {
	std::vector<unsigned char> fa = solid(1, 1, a, a, a);
	std::vector<unsigned char> fb = solid(1, 1, b, b, b);
	std::vector<const unsigned char*> src{fa.data(), fb.data()};
	std::vector<unsigned char> out;
	blend::mixRect(src, gamma, 4, 0, 0, 1, 1, out);		// stride: 1 pixel = 4 bytes
	return out.empty() ? 0 : out[0];
}

} // namespace

void test_blend() {
	unit::begin("blend");

	// ---- linear light, which is the entire point of the feature ----------
	//
	// Black and white averaged in linear light is not mid-grey. Averaging the
	// encoded bytes would answer 127 or 128, and a picture built that way is
	// visibly too dark. If this number ever comes back 127, the blend has
	// silently stopped doing the one thing it exists to do.
	CHECK_EQ((int)mixTwoGrey(0, 255, 2.2), 182);	// #B6
	CHECK_EQ((int)mixTwoGrey(0, 255, 2.4), 188);	// #BC

	// gamma 1 asks for no transfer at all, and then it IS the byte average.
	// Kept as a test because it is the control: it proves the 182 above comes
	// from the transfer and not from some unrelated bias.
	CHECK_EQ((int)mixTwoGrey(0, 255, 1.0), 127);

	// Order must not matter.
	CHECK_EQ((int)mixTwoGrey(255, 0, 2.2), (int)mixTwoGrey(0, 255, 2.2));

	// ---- idempotence over the whole range --------------------------------
	//
	// Blending a frame with itself must return that frame. This is what
	// catches a lossy round trip through the lookup tables, and a single
	// pinned digest cannot: a table that is wrong by one everywhere still
	// hashes consistently, so the digest agrees with itself forever.
	for (int v = 0; v < 256; v++) {
		const unsigned char c = (unsigned char)v;
		CHECK_EQ((int)mixTwoGrey(c, c, 2.2), v);
	}

	// ---- the rectangle, the stride and the alpha -------------------------
	{
		// `stride` is BYTES per line while x0 is in pixels, which is worth a
		// test of its own because the two are easy to mix up and the mistake
		// only shows once y0 is not zero. The window below starts on the
		// second row for exactly that reason.
		const int W = 4, H = 3;
		const int STRIDE = W * 4;
		std::vector<unsigned char> f = solid(W, H, 10, 20, 30);
		f[(1 * W + 2) * 4 + 0] = 200;		// x=2, y=1, red channel
		std::vector<const unsigned char*> one{f.data()};
		std::vector<unsigned char> out;
		blend::mixRect(one, 2.2, STRIDE, 1, 1, 2, 1, out);
		CHECK_EQ((int)out.size(), 2 * 1 * 4);
		CHECK_EQ((int)out[0], 10);		// x=1 untouched
		CHECK_EQ((int)out[4], 200);		// x=2 is the marked pixel
	}
	{
		// A single source is the raw path and must be an exact copy.
		std::vector<unsigned char> f = solid(2, 2, 1, 2, 3, 4);
		std::vector<const unsigned char*> one{f.data()};
		std::vector<unsigned char> out;
		blend::mixRect(one, 2.2, 2 * 4, 0, 0, 2, 2, out);
		CHECK(out == f);
	}
	{
		// Two sources go through the blend, which always writes an opaque
		// alpha whatever the inputs carried.
		std::vector<unsigned char> a = solid(1, 1, 8, 8, 8, 0x10);
		std::vector<unsigned char> b = solid(1, 1, 8, 8, 8, 0x20);
		std::vector<const unsigned char*> two{a.data(), b.data()};
		std::vector<unsigned char> out;
		blend::mixRect(two, 2.2, 4, 0, 0, 1, 1, out);
		CHECK_EQ((int)out[3], 0xff);
	}
	{
		// Degenerate requests must produce nothing rather than a crash or
		// a buffer of the wrong size.
		std::vector<unsigned char> f = solid(2, 2, 0, 0, 0);
		std::vector<const unsigned char*> one{f.data()};
		std::vector<unsigned char> out;
		blend::mixRect(one, 2.2, 2 * 4, 0, 0, 0, 2, out);
		CHECK(out.empty());
		blend::mixRect({}, 2.2, 2 * 4, 0, 0, 2, 2, out);
		CHECK(out.empty());
	}

	// ---- the ring ---------------------------------------------------------
	{
		const size_t BYTES = 4;
		blend::setDepth(3);
		CHECK_EQ(blend::depth(), 3);
		CHECK_EQ(blend::filled(), 0);
		CHECK(blend::history(2).empty());

		unsigned char f1[BYTES] = {1, 1, 1, 0xff};
		unsigned char f2[BYTES] = {2, 2, 2, 0xff};
		unsigned char f3[BYTES] = {3, 3, 3, 0xff};
		unsigned char f4[BYTES] = {4, 4, 4, 0xff};

		blend::push(f1, BYTES);
		blend::push(f2, BYTES);
		CHECK_EQ(blend::filled(), 2);

		// history() is newest first. If that order ever flips, a gigascreen
		// still blends to the same colour and nothing looks wrong, while
		// "the frame before last" quietly means the wrong frame.
		std::vector<const unsigned char*> h = blend::history(2);
		CHECK_EQ((int)h.size(), 2);
		CHECK_EQ((int)h[0][0], 2);
		CHECK_EQ((int)h[1][0], 1);

		// Asking for more than there is returns what there is.
		CHECK_EQ((int)blend::history(8).size(), 2);

		// Past the depth the oldest frame is gone, not the newest.
		blend::push(f3, BYTES);
		blend::push(f4, BYTES);
		CHECK_EQ(blend::filled(), 3);
		h = blend::history(3);
		CHECK_EQ((int)h[0][0], 4);
		CHECK_EQ((int)h[1][0], 3);
		CHECK_EQ((int)h[2][0], 2);

		// A changed frame size means the geometry moved, and the frames on
		// the far side of that are a different picture.
		unsigned char big[8] = {9, 9, 9, 0xff, 9, 9, 9, 0xff};
		blend::push(big, 8);
		CHECK_EQ(blend::filled(), 1);

		blend::clear();
		CHECK_EQ(blend::filled(), 0);
		CHECK(blend::history(1).empty());

		// Depth zero records nothing at all.
		blend::setDepth(0);
		blend::push(f1, BYTES);
		CHECK_EQ(blend::filled(), 0);

		// Out-of-range depths clamp rather than corrupt.
		blend::setDepth(999);
		CHECK_EQ(blend::depth(), blend::kMaxHistory);
		blend::setDepth(-4);
		CHECK_EQ(blend::depth(), 0);
	}

	// ---- distinctCount ----------------------------------------------------
	{
		unsigned char a[4] = {1, 1, 1, 0xff};
		unsigned char b[4] = {2, 2, 2, 0xff};
		unsigned char c[4] = {1, 1, 1, 0xff};		// equal to a
		CHECK_EQ(blend::distinctCount({a, b, c}, 4), 2);
		CHECK_EQ(blend::distinctCount({a, a, a}, 4), 1);
		CHECK_EQ(blend::distinctCount({}, 4), 0);
	}

	// ---- pattern ----------------------------------------------------------
	{
		const size_t BYTES = 4;
		unsigned char a[BYTES] = {1, 1, 1, 0xff};
		unsigned char b[BYTES] = {2, 2, 2, 0xff};
		unsigned char c[BYTES] = {3, 3, 3, 0xff};

		blend::setDepth(4);
		blend::clear();
		CHECK(blend::pattern(BYTES) == blend::Pattern::Unknown);
		blend::push(a, BYTES);
		blend::push(a, BYTES);
		CHECK(blend::pattern(BYTES) == blend::Pattern::Unknown);	// needs three
		blend::push(a, BYTES);
		CHECK(blend::pattern(BYTES) == blend::Pattern::Still);

		// A gigascreen: two pictures alternating, so the frame before last
		// matches and the one before it does not.
		blend::setDepth(0); blend::setDepth(4);
		blend::push(a, BYTES);
		blend::push(b, BYTES);
		blend::push(a, BYTES);
		CHECK(blend::pattern(BYTES) == blend::Pattern::Flicker);

		blend::setDepth(0); blend::setDepth(4);
		blend::push(a, BYTES);
		blend::push(b, BYTES);
		blend::push(c, BYTES);
		CHECK(blend::pattern(BYTES) == blend::Pattern::Animation);

		CHECK_EQ(blend::patternName(blend::Pattern::Flicker), std::string("flicker"));
		CHECK_EQ(blend::patternName(blend::Pattern::Unknown), std::string("unknown"));
	}

	// Leave the ring the way the server expects to find it.
	blend::setDepth(blend::defaults().history);
	blend::clear();
}
