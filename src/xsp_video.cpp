#include "xsp_video.h"

#include "xsp_blend.h"

#include <cstring>
#include <vector>

namespace xsp {
namespace video {

void applyGeometry(Computer* comp) {
	// zoom 1, no filters (xcore/vscalers.cpp:vid_upd_scale, else-branch)
	xstep = 1 << 8;
	ystep = 1 << 8;
	lefSkip = rigSkip = topSkip = botSkip = pixSkip = 0;
	// MainWin::updateWindow(), non-OpenGL path: bytesPerLine = window width * 4
	bytesPerLine = comp->vid->vsze.x * 4;
	bufSize = bytesPerLine * comp->vid->vsze.y;
	// Upstream's own antiflicker stays off for good: it blends destructively
	// into the buffer a screenshot and a digest both read, and history-dependent
	// pixels in the picture we call ground truth is not a trade worth making.
	// Blending happens on a copy instead - see xsp_blend.h.
	noflic = 0;
	// greyScale is left alone: it is a setting video_config owns, and a layout
	// change is no reason to undo what the caller asked for.
	blend::clear();		// frames drawn with the old geometry cannot be blended
}

// Which part of the frame buffer a capture covers. Shared so that a digest and
// a screenshot of the same frame never disagree about what "the frame" is.
namespace {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

Rect cropRect(Computer* comp, bool border) {
	Video* vid = comp->vid;
	Rect r;
	const int fullW = bytesPerLine / 4;
	const int fullH = vid->vsze.y;
	if (fullW <= 0 || fullH <= 0) return r;
	r.w = fullW;
	r.h = fullH;
	if (!border) {
		// paper starts at vid->bord, and the captured image starts at vid->lcut
		r.x = vid->bord.x - vid->lcut.x;
		r.y = vid->bord.y - vid->lcut.y;
		r.w = vid->scrn.x;
		r.h = vid->scrn.y;
		if (r.x < 0) r.x = 0;
		if (r.y < 0) r.y = 0;
		if (r.x + r.w > fullW) r.w = fullW - r.x;
		if (r.y + r.h > fullH) r.h = fullH - r.y;
	}
	return r;
}

} // namespace

// The frames a capture is made of, newest first. Normally the last completed
// frame alone; with blending, that one and the ones before it.
//
// bufimg holds the last completed frame and scrimg the one being drawn, so the
// history's newest entry and bufimg are the same picture - the history is read
// for all of them, and bufimg is the fallback for when it is empty or off.
namespace {

std::vector<const unsigned char*> sources(int blendFrames, BlendInfo* info) {
	if (blendFrames < 0) blendFrames = blend::defaults().frames;
	if (blendFrames < 1) blendFrames = 1;
	if (blendFrames > blend::kMaxHistory) blendFrames = blend::kMaxHistory;

	std::vector<const unsigned char*> src;
	if (blendFrames > 1) src = blend::history(blendFrames);
	if (src.empty()) src.push_back(bufimg);

	if (info) {
		info->frames = blendFrames;
		info->used = (int)src.size();
		info->gamma = blend::defaults().gamma;
		info->on = src.size() > 1;
		info->distinct = info->on ? blend::distinctCount(src, (size_t)bufSize)
					  : (int)src.size();
		info->pattern = blend::patternName(blend::pattern((size_t)bufSize));
	}
	return src;
}

} // namespace

Shot capture(Computer* comp, bool border, int scale, int blendFrames, BlendInfo* info) {
	Shot shot;
	const Rect rc = cropRect(comp, border);
	const int x0 = rc.x, y0 = rc.y, w = rc.w, h = rc.h;
	if (w <= 0 || h <= 0) return shot;

	if (scale < 1) scale = 1;
	if (scale > 8) scale = 8;

	std::vector<unsigned char> flat;		// w*h, one frame or the average
	blend::mixRect(sources(blendFrames, info), blend::defaults().gamma,
		       bytesPerLine, x0, y0, w, h, flat);
	if (flat.empty()) return shot;

	shot.width = w * scale;
	shot.height = h * scale;
	shot.rgba.resize((size_t)shot.width * shot.height * 4);

	for (int y = 0; y < h; y++) {
		const unsigned char* srow = flat.data() + (size_t)y * w * 4;
		for (int sy = 0; sy < scale; sy++) {
			unsigned char* drow = shot.rgba.data()
				+ ((size_t)(y * scale + sy) * shot.width) * 4;
			for (int x = 0; x < w; x++) {
				for (int sx = 0; sx < scale; sx++) {
					memcpy(drow + ((size_t)(x * scale + sx)) * 4, srow + (size_t)x * 4, 4);
				}
			}
		}
	}
	return shot;
}

void historyPush(Computer*) {
	if (bufSize > 0) blend::push(bufimg, (size_t)bufSize);
}

void historyClear() { blend::clear(); }

// ZX screen address of the pixel row `line` (0..191)
static int scrLineAddr(int base, int line) {
	int third = line >> 6;
	int row = (line >> 3) & 7;
	int sub = line & 7;
	return base + (third << 11) + (sub << 8) + (row << 5);
}

int displayedPage(Computer* comp) {
	return comp->vid->vidPage;
}

// The ULA's own read, copied from vid_mrd_cb in spectrum.c rather than invented:
// ramData indexed by MADR(page,adr) and masked with ramMask. Going through
// memRd() instead would be wrong, because that is the CPU's view - on a 128K
// bank 5 is always the one mapped at $4000, so a program showing bank 7 would
// still be reported as bank 5, which is the screen nobody is looking at.
int screenByte(Computer* comp, int page, int adr) {
	const int a = ((page & 0xff) << 14) + (adr & 0x3fff);
	return comp->mem->ramData[a & comp->mem->ramMask];
}

std::string screenText(Computer* comp, int page) {
	// font base: CHARS (23606/$5C36) points at font-256
	int chars = (memRd(comp->mem, 0x5c37) << 8) | memRd(comp->mem, 0x5c36);
	int fontBase = (chars + 256) & 0xffff;
	// sanity: the ZX font starts with 8 zero bytes (space)
	bool ok = true;
	for (int i = 0; i < 8; i++)
		if (memRd(comp->mem, fontBase + i)) { ok = false; break; }
	if (!ok) fontBase = 0x3d00;

	std::string out;
	for (int row = 0; row < 24; row++) {
		for (int col = 0; col < 32; col++) {
			unsigned char cell[8];
			for (int y = 0; y < 8; y++)
				cell[y] = (unsigned char)screenByte(comp, page, scrLineAddr(0, row * 8 + y) + col);
			char ch = '?';
			bool blank = true;
			for (int y = 0; y < 8; y++) if (cell[y]) blank = false;
			if (blank) {
				ch = ' ';
			} else {
				for (int c = 32; c < 128 && ch == '?'; c++) {
					int fa = fontBase + (c - 32) * 8;
					int y = 0;
					while (y < 8 && (unsigned char)memRd(comp->mem, fa + y) == cell[y]) y++;
					if (y == 8) ch = (char)c;
				}
				if (ch == '?') {		// try inverse video
					for (int c = 32; c < 128 && ch == '?'; c++) {
						int fa = fontBase + (c - 32) * 8;
						int y = 0;
						while (y < 8 && (unsigned char)(~memRd(comp->mem, fa + y) & 0xff) == cell[y]) y++;
						if (y == 8) ch = (char)c;
					}
				}
			}
			out += ch;
		}
		out += '\n';
	}
	return out;
}

// ---------------------------------------------------------------- digest
//
// MD5 (RFC 1321). Here for one reason only: the reference capture scripts a ZX
// project writes around this server hash their screens with md5, and a digest
// you can compare against theirs is worth forty lines of arithmetic.

namespace {

struct Md5 {
	unsigned int h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	unsigned long long len = 0;
	unsigned char buf[64];
	size_t fill = 0;

	static unsigned int rol(unsigned int x, int c) { return (x << c) | (x >> (32 - c)); }

	void block(const unsigned char* p) {
		static const unsigned int K[64] = {
			0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
			0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
			0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
			0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
			0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
			0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
			0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
			0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
		static const int S[64] = {
			7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
			5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
			4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
			6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
		unsigned int m[16];
		for (int i = 0; i < 16; i++)
			m[i] = (unsigned int)p[i * 4] | ((unsigned int)p[i * 4 + 1] << 8) |
			       ((unsigned int)p[i * 4 + 2] << 16) | ((unsigned int)p[i * 4 + 3] << 24);
		unsigned int a = h[0], b = h[1], c = h[2], d = h[3];
		for (int i = 0; i < 64; i++) {
			unsigned int f;
			int g;
			if (i < 16)      { f = (b & c) | (~b & d);      g = i; }
			else if (i < 32) { f = (d & b) | (~d & c);      g = (5 * i + 1) & 15; }
			else if (i < 48) { f = b ^ c ^ d;               g = (3 * i + 5) & 15; }
			else             { f = c ^ (b | ~d);            g = (7 * i) & 15; }
			unsigned int tmp = d;
			d = c;
			c = b;
			b = b + rol(a + f + K[i] + m[g], S[i]);
			a = tmp;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	}

	void add(const unsigned char* p, size_t n) {
		len += n;
		while (n) {
			size_t take = 64 - fill;
			if (take > n) take = n;
			memcpy(buf + fill, p, take);
			fill += take;
			p += take;
			n -= take;
			if (fill == 64) { block(buf); fill = 0; }
		}
	}

	std::string hex() {
		unsigned long long bits = len * 8;
		unsigned char pad = 0x80;
		add(&pad, 1);
		unsigned char zero = 0;
		while (fill != 56) add(&zero, 1);
		unsigned char tail[8];
		for (int i = 0; i < 8; i++) tail[i] = (unsigned char)((bits >> (8 * i)) & 0xff);
		add(tail, 8);
		char out[33];
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				snprintf(out + i * 8 + j * 2, 3, "%02x", (h[i] >> (8 * j)) & 0xff);
		return std::string(out, 32);
	}
};

} // namespace

std::string screenDigest(Computer* comp, const std::vector<int>& banks,
			 bool pixels, std::string& err) {
	const int kBank = 0x4000;
	const int kPixels = 0x1800;		// 6144 bytes of bitmap
	const int kAttrs = 0x300;		// 768 bytes of attributes
	Md5 md;
	for (int b : banks) {
		long long base = (long long)b * kBank;
		if (b < 0 || base + kBank > comp->mem->ramSize) {
			err = "bank " + std::to_string(b) + " is outside the machine's RAM (" +
			      std::to_string(comp->mem->ramSize / 1024) + "K)";
			return std::string();
		}
		const unsigned char* p = comp->mem->ramData + base;
		if (pixels) md.add(p, kPixels);
		md.add(p + kPixels, kAttrs);
	}
	return md.hex().substr(0, 12);
}

// The same digest over any stretch of memory. Read through the CPU's view, the
// way read_memory does, so `from`/`to` mean what every other address argument
// means and banked data follows whatever is paged in.
std::string memoryDigest(Computer* comp, int from, int to, std::string& err) {
	if (from < 0 || to < 0 || from > 0xffff || to > 0xffff) {
		err = "from and to must be within $0000-$FFFF";
		return std::string();
	}
	if (to < from) {
		err = "to is below from";
		return std::string();
	}
	Md5 md;
	std::vector<unsigned char> buf;
	buf.reserve((size_t)(to - from + 1));
	for (int a = from; a <= to; a++) buf.push_back((unsigned char)memRd(comp->mem, a));
	md.add(buf.data(), buf.size());
	return md.hex().substr(0, 12);
}

FrameHash frameDigest(Computer* comp, bool border, bool perLine,
		      int blendFrames, BlendInfo* info) {
	FrameHash out;
	const Rect rc = cropRect(comp, border);
	if (rc.w <= 0 || rc.h <= 0) return out;
	out.width = rc.w;
	out.height = rc.h;

	// the same pixels a screenshot with the same arguments would write, so the
	// digest and the PNG next to it describe one and the same picture
	std::vector<unsigned char> flat;
	blend::mixRect(sources(blendFrames, info), blend::defaults().gamma,
		       bytesPerLine, rc.x, rc.y, rc.w, rc.h, flat);
	if (flat.empty()) return out;

	const size_t rowBytes = (size_t)rc.w * 4;
	Md5 whole;
	if (perLine) out.lines.reserve((size_t)rc.h);
	for (int y = 0; y < rc.h; y++) {
		const unsigned char* row = flat.data() + (size_t)y * rowBytes;
		whole.add(row, rowBytes);
		if (perLine) {
			Md5 line;
			line.add(row, rowBytes);
			out.lines.push_back(line.hex().substr(0, 12));
		}
	}
	out.digest = whole.hex().substr(0, 12);
	return out;
}

std::string screenAttrs(Computer* comp, int page) {
	std::string out;
	char buf[32];
	for (int row = 0; row < 24; row++) {
		for (int col = 0; col < 32; col++) {
			int a = screenByte(comp, page, 0x1800 + row * 32 + col);
			snprintf(buf, sizeof(buf), "%02X ", a & 0xff);
			out += buf;
		}
		out += '\n';
	}
	return out;
}

} // namespace video
} // namespace xsp
