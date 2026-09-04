// The GIF recorder writes the file format itself, LZW included, so that a
// recording needs nothing installed. That makes it the one piece of output in
// this project that no library validates: a wrong sub-block length or a code
// width that grows one step late produces a file that some viewers open and
// others do not, and the difference is invisible from the outside.
//
// So this decodes what the encoder wrote, back to pixel indices, and compares.
#include "unit.h"
#include "xsp_record.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xsp;

namespace {

std::vector<unsigned char> readFile(const std::string& path) {
	std::vector<unsigned char> out;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return out;
	unsigned char buf[8192];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
	std::fclose(f);
	return out;
}

int le16(const std::vector<unsigned char>& v, size_t at) {
	return at + 1 < v.size() ? v[at] | (v[at + 1] << 8) : -1;
}

// One frame as it comes back out of the file.
struct Frame {
	int delay = 0;
	int width = 0, height = 0;
	std::vector<unsigned char> indices;
};

// A GIF LZW decoder, written to the specification rather than to the encoder,
// so that the two agreeing means something. The width grows when the next free
// code reaches the current limit, which is one code earlier than it looks:
// the decoder's table runs a step behind the encoder's.
bool lzwDecode(const std::vector<unsigned char>& file, size_t& at,
	       size_t pixels, std::vector<unsigned char>& out) {
	if (at >= file.size()) return false;
	const int minCodeSize = file[at++];
	if (minCodeSize < 2 || minCodeSize > 8) return false;

	// gather the sub-blocks into one stream
	std::vector<unsigned char> packed;
	while (at < file.size()) {
		const int len = file[at++];
		if (len == 0) break;
		if (at + (size_t)len > file.size()) return false;
		packed.insert(packed.end(), file.begin() + at, file.begin() + at + len);
		at += len;
	}

	const int clearCode = 1 << minCodeSize;
	const int endCode = clearCode + 1;
	std::vector<std::vector<unsigned char>> table;
	auto reset = [&]() {
		table.assign(endCode + 1, {});
		for (int i = 0; i < clearCode; i++) table[i] = {(unsigned char)i};
	};
	reset();

	int width = minCodeSize + 1;
	size_t bit = 0;
	int previous = -1;
	out.clear();

	auto readCode = [&](int& code) {
		if (bit + (size_t)width > packed.size() * 8) return false;
		code = 0;
		for (int i = 0; i < width; i++) {
			const size_t b = bit + i;
			if (packed[b >> 3] & (1u << (b & 7))) code |= 1 << i;
		}
		bit += width;
		return true;
	};

	while (true) {
		int code = 0;
		if (!readCode(code)) return false;
		if (code == endCode) break;
		if (code == clearCode) {
			reset();
			width = minCodeSize + 1;
			previous = -1;
			continue;
		}
		std::vector<unsigned char> entry;
		if (code < (int)table.size() && !table[code].empty()) {
			entry = table[code];
		} else if (previous >= 0) {
			// the code that is being defined by the very code that uses it
			entry = table[previous];
			entry.push_back(table[previous][0]);
		} else {
			return false;
		}
		out.insert(out.end(), entry.begin(), entry.end());
		if (previous >= 0) {
			std::vector<unsigned char> added = table[previous];
			added.push_back(entry[0]);
			table.push_back(added);
			if ((int)table.size() == (1 << width) && width < 12) width++;
		}
		previous = code;
		if (out.size() > pixels * 2) return false;	// runaway
	}
	return true;
}

// Walks the whole file: header, palette, loop block, then every frame.
struct Gif {
	bool ok = false;
	int width = 0, height = 0;
	int loopCount = -1;
	std::vector<unsigned char> palette;	// 256 * 3
	std::vector<Frame> frames;
};

Gif parseGif(const std::vector<unsigned char>& file) {
	Gif gif;
	if (file.size() < 13 + 768) return gif;
	if (std::memcmp(file.data(), "GIF89a", 6) != 0) return gif;
	gif.width = le16(file, 6);
	gif.height = le16(file, 8);
	if (file[10] != 0xf7) return gif;	// global table, 8 bits per pixel
	gif.palette.assign(file.begin() + 13, file.begin() + 13 + 768);

	size_t at = 13 + 768;
	int pendingDelay = -1;
	while (at < file.size()) {
		const unsigned char block = file[at];
		if (block == 0x3b) { gif.ok = true; break; }	// trailer
		if (block == 0x21) {
			if (at + 1 >= file.size()) return gif;
			const unsigned char label = file[at + 1];
			if (label == 0xf9) {			// graphic control
				if (at + 7 >= file.size()) return gif;
				pendingDelay = le16(file, at + 4);
				at += 8;
			} else if (label == 0xff) {		// NETSCAPE loop
				if (at + 18 >= file.size()) return gif;
				if (std::memcmp(&file[at + 3], "NETSCAPE2.0", 11) != 0) return gif;
				gif.loopCount = le16(file, at + 16);
				at += 19;
			} else {
				return gif;
			}
			continue;
		}
		if (block == 0x2c) {				// image descriptor
			if (at + 9 >= file.size()) return gif;
			Frame frame;
			frame.width = le16(file, at + 5);
			frame.height = le16(file, at + 7);
			if (file[at + 9] != 0x00) return gif;	// no local table
			frame.delay = pendingDelay;
			pendingDelay = -1;
			at += 10;
			if (!lzwDecode(file, at, (size_t)frame.width * frame.height,
				       frame.indices))
				return gif;
			gif.frames.push_back(frame);
			continue;
		}
		return gif;
	}
	return gif;
}

// The ZX palette as the core stores it: R | G<<8 | B<<16 | A<<24.
uint32_t rgba(int r, int g, int b) {
	return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xff000000u;
}

void pixel(std::vector<unsigned char>& buf, int r, int g, int b) {
	buf.push_back((unsigned char)r);
	buf.push_back((unsigned char)g);
	buf.push_back((unsigned char)b);
	buf.push_back(0xff);
}

} // namespace

void test_gif() {
	unit::begin("gif");

	unit::TempDir tmp("gif");

	// Four colours, chosen so that no two are near each other: a nearest-match
	// that went to the wrong entry would be obvious rather than plausible.
	std::vector<uint32_t> palette = {
		rgba(0, 0, 0), rgba(255, 0, 0), rgba(0, 255, 0), rgba(0, 0, 255),
	};

	// ---- one small frame, decoded back --------------------------------------
	{
		const std::string path = tmp.reserve("one.gif");
		record::GifWriter gif;
		std::string err;
		CHECK(gif.open(path, 4, 2, palette.data(), (int)palette.size(), 0, err));
		CHECK(err.empty());

		std::vector<unsigned char> frame;
		const int want[8] = {0, 1, 2, 3, 3, 2, 1, 0};
		for (int i = 0; i < 8; i++) {
			const uint32_t c = palette[want[i]];
			pixel(frame, c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
		}
		CHECK(gif.addFrame(frame.data(), 4, err));
		CHECK(err.empty());
		CHECK(gif.bytes() > 0);
		CHECK(gif.close());

		Gif parsed = parseGif(readFile(path));
		CHECK(parsed.ok);
		CHECK_EQ(parsed.width, 4);
		CHECK_EQ(parsed.height, 2);
		CHECK_EQ(parsed.loopCount, 0);		// 0 means forever
		CHECK_EQ((int)parsed.frames.size(), 1);
		if (!parsed.frames.empty()) {
			CHECK_EQ(parsed.frames[0].delay, 4);
			CHECK_EQ(parsed.frames[0].width, 4);
			CHECK_EQ(parsed.frames[0].height, 2);
			CHECK_EQ((int)parsed.frames[0].indices.size(), 8);
			for (int i = 0; i < 8 && i < (int)parsed.frames[0].indices.size(); i++)
				CHECK_EQ((int)parsed.frames[0].indices[i], want[i]);
		}

		// The emulator's palette became the global colour table verbatim, so
		// the colours in the file are the colours the ULA drew and not an
		// approximation of them. Entries past the ones given are black.
		if (parsed.palette.size() == 768) {
			CHECK_EQ((int)parsed.palette[3], 255);	// entry 1 = red
			CHECK_EQ((int)parsed.palette[4], 0);
			CHECK_EQ((int)parsed.palette[5], 0);
			CHECK_EQ((int)parsed.palette[7], 255);	// entry 2 = green
			CHECK_EQ((int)parsed.palette[11], 255);	// entry 3 = blue
			CHECK_EQ((int)parsed.palette[767], 0);	// unused, black
		}
	}

	// ---- colours that are not in the palette --------------------------------
	//
	// An exact entry wins over a near one, and a colour with no entry goes to
	// the closest by squared distance rather than to entry zero.
	{
		const std::string path = tmp.reserve("near.gif");
		record::GifWriter gif;
		std::string err;
		std::vector<uint32_t> close = {
			rgba(0, 0, 0), rgba(250, 0, 0), rgba(255, 0, 0),
		};
		CHECK(gif.open(path, 3, 1, close.data(), (int)close.size(), 0, err));

		std::vector<unsigned char> frame;
		pixel(frame, 255, 0, 0);	// exactly entry 2, not the nearby entry 1
		pixel(frame, 200, 10, 10);	// nearest is entry 1
		pixel(frame, 5, 5, 5);		// nearest is entry 0
		CHECK(gif.addFrame(frame.data(), 1, err));
		CHECK(gif.close());

		Gif parsed = parseGif(readFile(path));
		CHECK(parsed.ok);
		if (parsed.ok && parsed.frames.size() == 1 &&
		    parsed.frames[0].indices.size() == 3) {
			CHECK_EQ((int)parsed.frames[0].indices[0], 2);
			CHECK_EQ((int)parsed.frames[0].indices[1], 1);
			CHECK_EQ((int)parsed.frames[0].indices[2], 0);
		}
	}

	// ---- several frames -----------------------------------------------------
	{
		const std::string path = tmp.reserve("many.gif");
		record::GifWriter gif;
		std::string err;
		CHECK(gif.open(path, 2, 2, palette.data(), (int)palette.size(), 3, err));

		long long after1 = 0;
		for (int f = 0; f < 3; f++) {
			std::vector<unsigned char> frame;
			for (int i = 0; i < 4; i++) {
				const uint32_t c = palette[(f + i) % 4];
				pixel(frame, c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
			}
			// A zero delay is not a legal GIF frame time and every viewer
			// invents its own; clamp to the smallest that means something.
			CHECK(gif.addFrame(frame.data(), f == 0 ? 0 : 2, err));
			if (f == 0) after1 = gif.bytes();
		}
		CHECK(gif.bytes() > after1);		// each frame costs more bytes
		CHECK(gif.close());

		Gif parsed = parseGif(readFile(path));
		CHECK(parsed.ok);
		CHECK_EQ(parsed.loopCount, 3);
		CHECK_EQ((int)parsed.frames.size(), 3);
		if (parsed.frames.size() == 3) {
			CHECK_EQ(parsed.frames[0].delay, 1);	// clamped up from 0
			CHECK_EQ(parsed.frames[1].delay, 2);
			CHECK_EQ(parsed.frames[2].delay, 2);
			for (int f = 0; f < 3; f++)
				for (int i = 0; i < 4; i++)
					CHECK_EQ((int)parsed.frames[f].indices[i], (f + i) % 4);
		}
	}

	// ---- a frame big and varied enough to stretch the codes ------------------
	//
	// A ZX screen compresses into a few hundred codes and never leaves the
	// 9-bit width, so the interesting half of the encoder - growing to 12 bits
	// and then clearing the table - is exactly the half a screen recording
	// never reaches. This frame reaches it.
	{
		const std::string path = tmp.reserve("wide.gif");
		record::GifWriter gif;
		std::string err;

		std::vector<uint32_t> full;
		for (int i = 0; i < 256; i++) full.push_back(rgba(i, i, i));

		const int W = 200, H = 200;
		CHECK(gif.open(path, W, H, full.data(), (int)full.size(), 0, err));

		// A cheap deterministic sequence, not random: the test has to fail the
		// same way twice or it is not a test.
		std::vector<unsigned char> frame;
		std::vector<unsigned char> want;
		uint32_t state = 12345;
		for (int i = 0; i < W * H; i++) {
			state = state * 1103515245u + 12345u;
			const unsigned char v = (unsigned char)((state >> 16) & 0xff);
			want.push_back(v);
			pixel(frame, v, v, v);
		}
		CHECK(gif.addFrame(frame.data(), 5, err));
		CHECK(gif.close());

		Gif parsed = parseGif(readFile(path));
		CHECK(parsed.ok);
		CHECK_EQ((int)parsed.frames.size(), 1);
		if (!parsed.frames.empty()) {
			CHECK_EQ((int)parsed.frames[0].indices.size(), W * H);
			bool same = parsed.frames[0].indices.size() == want.size() &&
				std::memcmp(parsed.frames[0].indices.data(), want.data(),
					    want.size()) == 0;
			CHECK(same);
		}
	}

	// ---- refusing what cannot be written ------------------------------------
	{
		record::GifWriter gif;
		std::string err;
		const std::string path = tmp.reserve("never.gif");

		CHECK(!gif.open(path, 0, 10, palette.data(), 4, 0, err));
		CHECK(!err.empty());
		CHECK(!gif.open(path, 10, -1, palette.data(), 4, 0, err));
		CHECK(!gif.open(path, 70000, 10, palette.data(), 4, 0, err));
		CHECK(err.find("large") != std::string::npos);

		// A directory that does not exist is a failure, not a silent recording
		// into nowhere that reports success for a minute and then has no file.
		CHECK(!gif.open(unit::TempDir("gif-missing").path() + "/nope/x.gif",
				10, 10, palette.data(), 4, 0, err));

		// Adding to a recorder that never opened says so.
		std::vector<unsigned char> pixels(4 * 4, 0);
		CHECK(!gif.addFrame(pixels.data(), 1, err));
		CHECK(err.find("not open") != std::string::npos);

		// Closing one that is not open is not an error: it is what the
		// destructor does after an explicit close.
		CHECK(gif.close());
		CHECK(gif.close());
	}

	// A palette size outside 1..256 is clamped rather than read past the end of
	// the caller's array or written as a short colour table.
	{
		const std::string path = tmp.reserve("clamped.gif");
		record::GifWriter gif;
		std::string err;
		CHECK(gif.open(path, 2, 1, palette.data(), 0, 0, err));
		std::vector<unsigned char> frame;
		pixel(frame, 200, 200, 200);
		pixel(frame, 0, 0, 0);
		CHECK(gif.addFrame(frame.data(), 1, err));
		CHECK(gif.close());
		Gif parsed = parseGif(readFile(path));
		CHECK(parsed.ok);
		// One usable entry, so both pixels have to be it.
		if (parsed.ok && parsed.frames.size() == 1 &&
		    parsed.frames[0].indices.size() == 2) {
			CHECK_EQ((int)parsed.frames[0].indices[0], 0);
			CHECK_EQ((int)parsed.frames[0].indices[1], 0);
		}
	}

	// ---- choosing a colour table --------------------------------------------
	//
	// A raw ZX frame only ever holds palette entries, so the emulator's own
	// palette is exact for it. A blended frame does not: its colours are
	// averages that are in no palette by design, and handing GIF the ZX one
	// would map every mixed colour back to the nearest pure one - throwing away
	// exactly the mixing the caller asked to see.
	{
		std::vector<unsigned char> few;
		pixel(few, 0, 0, 0);
		pixel(few, 255, 0, 0);
		pixel(few, 0, 0, 0);
		pixel(few, 8, 8, 8);
		bool exact = false;
		std::vector<uint32_t> pal = record::picturePalette(few, exact);
		CHECK(exact);				// three colours fit in 256
		CHECK_EQ((int)pal.size(), 3);

		// The alpha channel is not a colour: two pixels differing only in it
		// are one entry, or a blended frame would exhaust the table on
		// transparency nobody asked about.
		std::vector<unsigned char> alpha;
		alpha.insert(alpha.end(), {10, 20, 30, 0xff});
		alpha.insert(alpha.end(), {10, 20, 30, 0x40});
		bool exactAlpha = false;
		CHECK_EQ((int)record::picturePalette(alpha, exactAlpha).size(), 1);
		CHECK(exactAlpha);
	}

	// Exactly 256 colours still fit; 257 do not, and then the table is the most
	// used ones rather than the first ones encountered - which is the whole
	// difference between a picture that looks right and one whose rarest colour
	// survived at the expense of its commonest.
	{
		std::vector<unsigned char> full;
		for (int i = 0; i < 256; i++) pixel(full, i, 0, 0);
		bool exact = false;
		CHECK_EQ((int)record::picturePalette(full, exact).size(), 256);
		CHECK(exact);

		std::vector<unsigned char> over;
		// One colour used many times, then 300 used once each.
		for (int i = 0; i < 50; i++) pixel(over, 1, 2, 3);
		for (int i = 0; i < 300; i++) pixel(over, i & 0xff, (i >> 8) + 10, 7);
		bool exactOver = true;
		std::vector<uint32_t> pal = record::picturePalette(over, exactOver);
		CHECK(!exactOver);
		CHECK_EQ((int)pal.size(), 256);
		// The commonest colour has to be in it.
		const uint32_t common = 1u | (2u << 8) | (3u << 16);
		bool kept = false;
		for (uint32_t c : pal) if (c == common) kept = true;
		CHECK(kept);
	}

	// An empty picture still yields a table, because a GIF with none is not a
	// GIF and the writer would be handed a null pointer.
	{
		std::vector<unsigned char> nothing;
		bool exact = false;
		CHECK_EQ((int)record::picturePalette(nothing, exact).size(), 1);
	}

	// The destructor closes an open recorder, so a recording that ends by the
	// object going out of scope still produces a file a viewer will open.
	{
		const std::string path = tmp.reserve("scoped.gif");
		{
			record::GifWriter gif;
			std::string err;
			CHECK(gif.open(path, 2, 1, palette.data(), 4, 0, err));
			std::vector<unsigned char> frame;
			pixel(frame, 0, 0, 0);
			pixel(frame, 255, 0, 0);
			CHECK(gif.addFrame(frame.data(), 1, err));
		}
		Gif parsed = parseGif(readFile(path));
		CHECK(parsed.ok);		// the trailer was written
		CHECK_EQ((int)parsed.frames.size(), 1);
	}
}
