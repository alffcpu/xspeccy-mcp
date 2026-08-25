#include "xsp_blend.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace xsp {
namespace blend {

namespace {

// ---- linear light -----------------------------------------------------
//
// Two tables and no floating point in the inner loop: encoded byte -> linear
// (16 bit, so averaging three frames loses nothing) and back. 64K of table,
// rebuilt only when gamma changes.

constexpr int kLinMax = 65535;

uint32_t g_lin[256];
uint8_t g_inv[kLinMax + 1];
double g_lutGamma = -1;

void buildLut(double gamma) {
	if (g_lutGamma == gamma) return;
	if (gamma <= 1.0) {
		// no transfer at all: average the sRGB bytes, which is what a naive
		// blend does and what a caller asking for gamma 1 is asking for
		for (int i = 0; i < 256; i++) g_lin[i] = (uint32_t)i * 257;
		for (int v = 0; v <= kLinMax; v++) g_inv[v] = (uint8_t)((v * 255 + 32767) / kLinMax);
		g_lutGamma = gamma;
		return;
	}
	for (int i = 0; i < 256; i++) {
		const double c = i / 255.0;
		const double v = (c <= 0.04045) ? c / 12.92 : std::pow((c + 0.055) / 1.055, gamma);
		g_lin[i] = (uint32_t)(v * kLinMax + 0.5);
	}
	for (int i = 0; i <= kLinMax; i++) {
		const double v = (double)i / kLinMax;
		const double c = (v <= 0.0031308) ? 12.92 * v
						  : 1.055 * std::pow(v, 1.0 / gamma) - 0.055;
		int b = (int)(c * 255.0 + 0.5);
		if (b < 0) b = 0;
		if (b > 255) b = 255;
		g_inv[i] = (uint8_t)b;
	}
	g_lutGamma = gamma;
}

// ---- the ring ---------------------------------------------------------

std::vector<unsigned char> g_ring;	// depth * g_bytes, oldest overwritten first
size_t g_bytes = 0;
int g_depth = 0;
int g_head = -1;			// slot holding the newest frame
int g_filled = 0;

Settings g_defaults;

} // namespace

Settings& defaults() { return g_defaults; }

void clear() {
	g_head = -1;
	g_filled = 0;
}

void setDepth(int d) {
	if (d < 0) d = 0;
	if (d > kMaxHistory) d = kMaxHistory;
	if (d == g_depth) return;
	g_depth = d;
	g_ring.clear();
	g_ring.shrink_to_fit();
	g_bytes = 0;
	clear();
}

int depth() { return g_depth; }
int filled() { return g_filled; }

void push(const unsigned char* frame, size_t bytes) {
	if (!g_depth || !frame || !bytes) return;
	if (bytes != g_bytes) {			// first frame, or the geometry moved
		g_bytes = bytes;
		g_ring.assign((size_t)g_depth * bytes, 0);
		clear();
	}
	g_head = (g_head + 1) % g_depth;
	memcpy(g_ring.data() + (size_t)g_head * g_bytes, frame, g_bytes);
	if (g_filled < g_depth) g_filled++;
}

std::vector<const unsigned char*> history(int want) {
	std::vector<const unsigned char*> out;
	if (g_head < 0 || !g_bytes) return out;
	if (want > g_filled) want = g_filled;
	for (int i = 0; i < want; i++) {
		const int slot = (g_head - i + g_depth * 2) % g_depth;
		out.push_back(g_ring.data() + (size_t)slot * g_bytes);
	}
	return out;
}

int distinctCount(const std::vector<const unsigned char*>& src, size_t bytes) {
	int n = 0;
	for (size_t i = 0; i < src.size(); i++) {
		bool dup = false;
		for (size_t j = 0; j < i && !dup; j++)
			dup = (memcmp(src[i], src[j], bytes) == 0);
		if (!dup) n++;
	}
	return n;
}

Pattern pattern(size_t bytes) {
	// Three frames is the fewest that can tell the two apart: with two, "they
	// differ" is all there is to say.
	std::vector<const unsigned char*> f = history(3);
	if (f.size() < 3 || !bytes) return Pattern::Unknown;
	const bool prevSame = memcmp(f[0], f[1], bytes) == 0;
	const bool skipSame = memcmp(f[0], f[2], bytes) == 0;
	if (prevSame && skipSame) return Pattern::Still;
	if (!prevSame && skipSame) return Pattern::Flicker;	// alternates with period 2
	return Pattern::Animation;
}

const char* patternName(Pattern p) {
	switch (p) {
	case Pattern::Still: return "still";
	case Pattern::Flicker: return "flicker";
	case Pattern::Animation: return "animation";
	default: return "unknown";
	}
}

void mixRect(const std::vector<const unsigned char*>& src, double gamma,
	     int stride, int x0, int y0, int w, int h,
	     std::vector<unsigned char>& out) {
	if (w <= 0 || h <= 0 || src.empty()) { out.clear(); return; }
	const int n = (int)src.size();
	const size_t rowBytes = (size_t)w * 4;
	out.resize(rowBytes * h);

	if (n == 1) {				// the raw path
		for (int y = 0; y < h; y++)
			memcpy(out.data() + (size_t)y * rowBytes,
			       src[0] + (size_t)(y0 + y) * stride + (size_t)x0 * 4, rowBytes);
		return;
	}

	buildLut(gamma);
	// divide by n without a division per channel
	const uint64_t recip = ((1ull << 32) + n - 1) / n;

	for (int y = 0; y < h; y++) {
		unsigned char* dst = out.data() + (size_t)y * rowBytes;
		const size_t off = (size_t)(y0 + y) * stride + (size_t)x0 * 4;
		for (int x = 0; x < w; x++) {
			const unsigned char* p0 = src[0] + off + (size_t)x * 4;
			// a static pixel is the same in every frame, and most of a
			// screen is static: check before touching the tables
			bool same = true;
			for (int i = 1; i < n && same; i++)
				same = (memcmp(src[i] + off + (size_t)x * 4, p0, 3) == 0);
			if (same) {
				dst[0] = p0[0]; dst[1] = p0[1]; dst[2] = p0[2]; dst[3] = 0xff;
				dst += 4;
				continue;
			}
			for (int ch = 0; ch < 3; ch++) {
				uint32_t sum = 0;
				for (int i = 0; i < n; i++)
					sum += g_lin[src[i][off + (size_t)x * 4 + ch]];
				const uint32_t avg = (uint32_t)(((uint64_t)sum * recip) >> 32);
				dst[ch] = g_inv[avg > kLinMax ? kLinMax : avg];
			}
			dst[3] = 0xff;
			dst += 4;
		}
	}
}

} // namespace blend
} // namespace xsp
