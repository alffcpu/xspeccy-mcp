#include "unit.h"
#include "xsp_video.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

using namespace xsp;

namespace {

std::vector<unsigned char> readFile(const std::string& path) {
	std::vector<unsigned char> out;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return out;
	unsigned char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
	std::fclose(f);
	return out;
}

uint32_t be32(const unsigned char* p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

} // namespace

void test_png() {
	unit::begin("png");

	// A 3x2 image with every pixel different, so a row that comes back in the
	// wrong order or a channel that is swapped cannot pass by coincidence.
	const int W = 3, H = 2;
	std::vector<unsigned char> rgba;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			rgba.push_back((unsigned char)(10 + x));
			rgba.push_back((unsigned char)(20 + y));
			rgba.push_back((unsigned char)(30 + x * H + y));
			rgba.push_back(0xff);
		}

	const std::string path = std::string(P_tmpdir) + "/xsp-unit.png";
	std::string err;
	CHECK(writePng(path, rgba.data(), W, H, err));
	CHECK(err.empty());

	std::vector<unsigned char> png = readFile(path);
	CHECK(png.size() > 8);
	if (png.size() < 8) return;

	// ---- the signature ----------------------------------------------------
	const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	CHECK(std::memcmp(png.data(), sig, 8) == 0);

	// ---- walk the chunks --------------------------------------------------
	//
	// Every chunk carries its own CRC. Checking them is what turns "the file
	// exists and starts with PNG" - which is all the end-to-end suite can
	// afford to ask - into "a decoder will accept this".
	bool sawIHDR = false, sawIEND = false;
	int gotW = 0, gotH = 0, bitDepth = 0, colourType = 0;
	std::vector<unsigned char> idat;
	size_t p = 8;
	while (p + 12 <= png.size()) {
		const uint32_t len = be32(&png[p]);
		if (p + 12 + len > png.size()) { CHECK(false); break; }
		const char* type = (const char*)&png[p + 4];
		const unsigned char* data = &png[p + 8];

		uLong crc = crc32(0, (const Bytef*)type, 4);
		if (len) crc = crc32(crc, (const Bytef*)data, (uInt)len);
		CHECK_EQ((unsigned)be32(&png[p + 8 + len]), (unsigned)crc);

		if (std::memcmp(type, "IHDR", 4) == 0) {
			sawIHDR = true;
			CHECK_EQ((int)len, 13);
			gotW = (int)be32(data);
			gotH = (int)be32(data + 4);
			bitDepth = data[8];
			colourType = data[9];
		} else if (std::memcmp(type, "IDAT", 4) == 0) {
			idat.insert(idat.end(), data, data + len);
		} else if (std::memcmp(type, "IEND", 4) == 0) {
			sawIEND = true;
		}
		p += 12 + len;
	}
	CHECK(sawIHDR);
	CHECK(sawIEND);
	CHECK_EQ(p, png.size());		// no trailing rubbish
	CHECK_EQ(gotW, W);
	CHECK_EQ(gotH, H);
	CHECK_EQ(bitDepth, 8);
	CHECK_EQ(colourType, 6);		// RGBA

	// ---- the pixels come back ---------------------------------------------
	{
		std::vector<unsigned char> raw((size_t)H * (W * 4 + 1) + 64);
		uLongf outLen = (uLongf)raw.size();
		const int rc = uncompress(raw.data(), &outLen, idat.data(), (uLong)idat.size());
		CHECK_EQ(rc, Z_OK);
		CHECK_EQ((int)outLen, H * (W * 4 + 1));

		bool same = true;
		for (int y = 0; y < H && same; y++) {
			const unsigned char* row = raw.data() + (size_t)y * (W * 4 + 1);
			if (row[0] != 0) same = false;		// filter 0 = none
			else if (std::memcmp(row + 1, rgba.data() + (size_t)y * W * 4,
					     (size_t)W * 4) != 0) same = false;
		}
		CHECK(same);
	}

	std::remove(path.c_str());

	// ---- refusals ---------------------------------------------------------
	err.clear();
	CHECK(!writePng(path, rgba.data(), 0, H, err));
	CHECK(!err.empty());
	err.clear();
	CHECK(!writePng(path, rgba.data(), W, -1, err));
	CHECK(!err.empty());
}
