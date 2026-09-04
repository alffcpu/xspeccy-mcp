// Minimal PNG writer (RGBA8, filter 0, zlib deflate). Avoids pulling in a
// whole image library for what is a handful of chunks.
#include "xsp_video.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <zlib.h>

namespace xsp {

static void put32(std::vector<unsigned char>& v, uint32_t x) {
	v.push_back((unsigned char)(x >> 24));
	v.push_back((unsigned char)(x >> 16));
	v.push_back((unsigned char)(x >> 8));
	v.push_back((unsigned char)x);
}

static void putChunk(FILE* f, const char* type, const unsigned char* data, size_t len) {
	std::vector<unsigned char> head;
	put32(head, (uint32_t)len);
	fwrite(head.data(), 1, head.size(), f);
	fwrite(type, 1, 4, f);
	if (len) fwrite(data, 1, len, f);
	uLong crc = crc32(0, (const Bytef*)type, 4);
	if (len) crc = crc32(crc, (const Bytef*)data, (uInt)len);
	std::vector<unsigned char> tail;
	put32(tail, (uint32_t)crc);
	fwrite(tail.data(), 1, tail.size(), f);
}

bool writePng(const std::string& path, const unsigned char* rgba,
	      int width, int height, std::string& err) {
	if (width <= 0 || height <= 0) { err = "empty image"; return false; }

	// raw stream: one filter byte (0 = none) per scanline
	std::vector<unsigned char> raw;
	raw.reserve((size_t)height * (width * 4 + 1));
	for (int y = 0; y < height; y++) {
		raw.push_back(0);
		raw.insert(raw.end(), rgba + (size_t)y * width * 4,
			   rgba + (size_t)(y + 1) * width * 4);
	}

	uLongf compLen = compressBound((uLong)raw.size());
	std::vector<unsigned char> comp(compLen);
	if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 6) != Z_OK) {
		err = "zlib compress failed";
		return false;
	}
	comp.resize(compLen);

	FILE* f = fopen(path.c_str(), "wb");
	if (!f) { err = "can't write '" + path + "'"; return false; }

	static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	fwrite(sig, 1, 8, f);

	std::vector<unsigned char> ihdr;
	put32(ihdr, (uint32_t)width);
	put32(ihdr, (uint32_t)height);
	ihdr.push_back(8);	// bit depth
	ihdr.push_back(6);	// colour type: RGBA
	ihdr.push_back(0);	// deflate
	ihdr.push_back(0);	// filter method
	ihdr.push_back(0);	// no interlace
	putChunk(f, "IHDR", ihdr.data(), ihdr.size());
	putChunk(f, "IDAT", comp.data(), comp.size());
	putChunk(f, "IEND", nullptr, 0);

	// fclose flushes, so a full disk fails there rather than in any of the
	// writes above: asking ferror first and closing without looking would
	// report a truncated file as a written one.
	bool ok = !ferror(f);
	if (fclose(f) != 0) ok = false;
	if (!ok) err = "write error on '" + path + "'";
	return ok;
}

} // namespace xsp
