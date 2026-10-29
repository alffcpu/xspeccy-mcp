// Files: snapshots, tapes and disks.
#include "tools/common.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include <zlib.h>

extern "C" {
#include "filetypes/unpackers/lh1/public/lha_decoder.h"
}
#include "xstate.h"

// the core's hardware table, for resolving a definition's `hw` name to an id
// without switching to it (media.cpp is C++, the table lives in C)
extern "C" tabHwItem tabHwPtr[];

namespace xsp {
namespace tools {

// ------------------------------------------------------------ .spg/.z80 packers
//
// The compressed blocks inside an .spg are unpacked by two tiny decoders the
// core ships as-is: demegalz and dehrust. Both read the input through a
// bbStream whose declared length is hard-coded to 0xffff while the block
// buffer is 0x4000 - a stream that never terminates reads 48K past the buffer
// end - and write their output with no bound check at all: a hostile length
// field walks straight through the 64K output buffer and on.
//
// The fix cannot run inside the decoder, so the same algorithms run here in
// bounded form first: every input read stops at the real block size and every
// output write stays inside the 64K the core's buffer really is. A stream the
// bounded twin cannot finish is one the core's copy would have finished only
// by reading out of bounds - it is rejected instead.
namespace {

const int kPackedOutMax = 0xffff;	// outbuf in loadSPG

// bbStream, bound to the bytes that actually exist. idx/bits/pType replicate
// the upstream state machine bit for bit; only `len` means the file's real
// size rather than the unconditional 0xffff upstream reads up to.
struct Bs {
	const unsigned char* base;
	const unsigned char* p;
	const unsigned char* end;
	int idx;
	int pType;
	bool eof;
	unsigned short bits;
};

const unsigned short kBsMask[] = {0x8000, 0x4000, 0x2000, 0x1000, 0x800,
				  0x400, 0x200, 0x100, 0x80, 0x40, 0x20, 0x10,
				  0x8, 0x4, 0x2, 0x1};

void bsInit(Bs& bs, const unsigned char* from, size_t sze, int type) {
	bs.base = bs.p = from;
	bs.end = from + sze;
	bs.pType = type;
	bs.eof = false;
	bs.bits = 0;
	if (type == 1) {			// PAK_HRS
		bs.idx = 0;
		bs.bits = 0;
	} else {				// PAK_MLZ
		bs.idx = 16;
	}
}

unsigned char bsGetByte(Bs& bs) {
	if (bs.p >= bs.end) { bs.eof = true; return 0; }
	return *bs.p++;
}

unsigned char bsGetBit(Bs& bs) {
	if (bs.idx > 15 && bs.pType == 2) {	// PAK_MLZ
		bs.bits = bsGetByte(bs);
		bs.idx = 8;
	}
	unsigned char bit = (bs.bits & kBsMask[bs.idx]) ? 1 : 0;
	if (bs.idx == 15 && bs.pType == 1) {	// PAK_HRS
		bs.bits = bsGetByte(bs);
		bs.bits |= (bsGetByte(bs) << 8);
		bs.idx = -1;
	}
	bs.idx++;
	return bit;
}

unsigned char bsGetBits(Bs& bs, int n) {
	unsigned char r = 0;
	do { r = (r << 1) | bsGetBit(bs); } while (--n);
	return r;
}

// Outbound cursor with the buffer the core really gives the decoder.
struct Out {
	unsigned char* dst;			// start of the 64K buffer
	unsigned char* to;
	size_t cap;
	bool overflow() const { return to >= dst + cap; }
	// match-copy source must sit in what was written so far
	bool read(int off) const { return to + off >= dst && to + off < to; }
};

void outByte(Out& o, unsigned char v, bool& ok) {
	if (o.overflow()) { ok = false; return; }
	*o.to++ = v;
}

void outMatch(Out& o, int off, bool& ok) {
	if (o.overflow() || !o.read(off)) { ok = false; return; }
	*o.to = o.to[off];
	o.to++;
}

// demegalz(), bounds-checked: identical control flow, each read/write proven.
// Returns the decompressed size, or -1 when the stream cannot be unpacked
// inside the buffers the core allocates.
int checkMlz(const unsigned char* src, size_t sze) {
	static thread_local std::vector<unsigned char> buf(kPackedOutMax + 512);
	Out o{buf.data(), buf.data(), kPackedOutMax};
	Bs bs;
	bsInit(bs, src, sze, 2);		// PAK_MLZ
	bool ok = true;
	// upstream seeds the bits register from the first two input bytes
	// through bsGetByte inside bsGetBit; the literal first byte is a byte
	outByte(o, bsGetByte(bs), ok);
	int work = 1;
	while (!bs.eof && work && ok) {
		while (ok && bsGetBit(bs)) outByte(o, bsGetByte(bs), ok);
		unsigned char len = bsGetBits(bs, 2);
		signed short offset = 0;
		switch (len) {
			case 0x00:
				offset = (signed short)bsGetBits(bs, 3) - 8;
				len = 1;
				break;
			case 0x01:
				offset = (signed short)bsGetByte(bs) - 256;
				len = 2;
				break;
			case 0x02:
				if (bsGetBit(bs)) {
					offset = (signed short)((0xf0 + bsGetBits(bs, 4) - 1) << 8);
					offset += bsGetByte(bs);
				} else {
					offset = (signed short)bsGetByte(bs) - 256;
				}
				len = 3;
				break;
			default:
				len = 1;
				while (!bsGetBit(bs) && len < 9) len++;
				switch (len) {
					case 1: len = 4 + bsGetBit(bs); break;
					case 2: len = 6 + bsGetBits(bs, 2); break;
					case 3: len = 10 + bsGetBits(bs, 3); break;
					case 4: len = 18 + bsGetBits(bs, 4); break;
					case 5: len = 34 + bsGetBits(bs, 5); break;
					case 6: len = 66 + bsGetBits(bs, 6); break;
					case 7:
						offset = 130 + bsGetBits(bs, 7);
						len = (offset < 255) ? (offset & 0xff) : 0xff;
						break;
					default: len = 0; work = 0; break;
				}
				if (bsGetBit(bs)) {
					offset = (signed short)((0xf0 + bsGetBits(bs, 4) - 1) << 8);
					offset += bsGetByte(bs);
				} else {
					offset = (signed short)bsGetByte(bs) - 256;
				}
				break;
		}
		while (len > 0 && ok) { outMatch(o, offset, ok); len--; }
	}
	// a stream that asked for more input than the block holds is the one the
	// core's decoder would have kept reading out of the 16K buffer
	return (bs.eof || !ok) ? -1 : (int)(o.to - o.dst);
}

// dehrust(), bounds-checked the same way.
int checkHrs(const unsigned char* src, size_t sze) {
	static thread_local std::vector<unsigned char> buf(kPackedOutMax + 512);
	Out o{buf.data(), buf.data(), kPackedOutMax};
	Bs bs;
	bsInit(bs, src, sze, 1);		// PAK_HRS
	bool ok = true;
	// PAK_HRS seeds the bit register with the first two input bytes
	bs.bits = bsGetByte(bs);
	bs.bits |= (bsGetByte(bs) << 8);
	outByte(o, bsGetByte(bs), ok);
	unsigned char noBits = 2;
	static const unsigned char mask[] = {0, 0, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x80, 0};

	while (!bs.eof && ok) {
		while (ok && bsGetBit(bs)) outByte(o, bsGetByte(bs), ok);
		unsigned short len = 0;
		unsigned char bb;
		do {
			bb = bsGetBits(bs, 2);
			len += bb;
		} while (bb == 0x03 && len != 0x0f);

		short offset = 0;
		if (len == 0) {
			offset = 0xfff8 + bsGetBits(bs, 3);
			outMatch(o, offset, ok);
			continue;
		}
		if (len == 1) {
			unsigned char code = bsGetBits(bs, 2);
			if (code == 2) {
				unsigned char b = bsGetByte(bs);
				if (b >= 0xe0) {
					b <<= 1; ++b;
					b ^= 2;
					if (b == 0xff) {
						// mask[] upstream has 9 entries; a stream that
						// stretches noBits past it reads off the table
						if (++noBits > 8) return -1;
						continue;
					}
					offset = 0xff00 + b - 0x0f;
					outMatch(o, offset, ok);
					outByte(o, bsGetByte(bs), ok);
					outMatch(o, offset, ok);
					continue;
				}
				offset = 0xff00 + b;
			}
			if (code == 0 || code == 1) {
				offset = bsGetByte(bs);
				offset += 256 * (code ? 0xfe : 0xfd);
			}
			if (code == 3) offset = 0xffe0 + bsGetBits(bs, 5);
			for (int i = 0; i < 2; i++) outMatch(o, offset, ok);
			continue;
		}
		if (len == 3) {
			if (bsGetBit(bs)) {
				offset = 0xfff0 + bsGetBits(bs, 4);
				outMatch(o, offset, ok);
				outByte(o, bsGetByte(bs), ok);
				outMatch(o, offset, ok);
				continue;
			}
			if (bsGetBit(bs)) {
				int noBytes = 6 + bsGetBits(bs, 4);
				for (int i = 0; i < 2 * noBytes && ok; i++)
					outByte(o, bsGetByte(bs), ok);
				continue;
			}
			len = bsGetBits(bs, 7);
			if (len == 0x0f) break;		// EOF marker
			if (len < 0x0f) len = 256 * len + bsGetByte(bs);
		}
		if (len == 2) ++len;
		unsigned char code = bsGetBits(bs, 2);
		if (code == 1) {
			unsigned char b = bsGetByte(bs);
			if (b >= 0xe0) {
				if (len > 3) return -1;
				b <<= 1; ++b;
				b ^= 3;
				offset = 0xff00 + b - 0x0f;
				outMatch(o, offset, ok);
				outByte(o, bsGetByte(bs), ok);
				outMatch(o, offset, ok);
				continue;
			}
			offset = 0xff00 + b;
		}
		if (code == 0) offset = 0xfe00 + bsGetByte(bs);
		if (code == 2) offset = 0xffe0 + bsGetBits(bs, 5);
		if (code == 3) {
			offset = 256 * (mask[noBits] + bsGetBits(bs, noBits));
			offset += bsGetByte(bs);
		}
		for (unsigned i = 0; i < len && ok; i++) outMatch(o, offset, ok);
	}
	return (bs.eof || !ok) ? -1 : (int)(o.to - o.dst);
}

} // namespace

// loadSPG trusts the file: a 16-bit block count walks a fixed 768-byte
// descriptor table, every payload fread goes unchecked, and a compressed
// block is handed to a decoder that reads past the input and writes past the
// output when the stream lies. Walk it all here, where a bad file is still
// just an error.
static void checkSpgFile(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	unsigned char head[0x100], tab[768];
	size_t got = fread(head, 1, sizeof(head), f);
	// a bad signature or version is refused here, not handed to the core:
	// the answer would be the same error either way, but returning would let
	// load_file switch the machine first and leave it switched on the error
	if (got < sizeof(head) ||
	    memcmp(head + 0x20, "SpectrumProg", 12) != 0 || head[0x2c] != 0x10) {
		fclose(f);
		throw std::runtime_error(
			"bad .spg: no 'SpectrumProg' signature/version at +$20");
	}
	if (fread(tab, 1, sizeof(tab), f) < sizeof(tab)) {
		fclose(f);
		throw std::runtime_error("truncated .spg: no room for the block table");
	}
	const int blkCount = head[0x3a] | (head[0x3b] << 8);
	if (blkCount > 256) {
		fclose(f);
		throw std::runtime_error("bad .spg: block count " + std::to_string(blkCount) +
					 " walks past the 256-entry block table");
	}
	long long need = sizeof(head) + sizeof(tab);
	std::vector<int> sizes;
	int idx = 0;
	for (int i = 0; i < blkCount; i++) {
		int sze = ((tab[idx + 1] & 0x1f) + 1) << 9;
		sizes.push_back(sze);
		need += sze;
		if (tab[idx] & 0x80) break;	// last block flag
		idx += 3;
	}
	fseek(f, 0, SEEK_END);
	const long fsize = ftell(f);
	if (fsize < need) {
		fclose(f);
		throw std::runtime_error("truncated .spg: declares " +
					 std::to_string(need) + " bytes, file has " +
					 std::to_string(fsize));
	}
	// the payloads sit back-to-back after the table; decode each compressed
	// one the bounded way before the core's own decoder ever sees it. The
	// stream must be rewound first - the size probe above left it at EOF,
	// where a fread that gets nothing would end validation silently
	fseek(f, (long)(sizeof(head) + sizeof(tab)), SEEK_SET);
	idx = 0;
	std::vector<unsigned char> blk;
	for (size_t i = 0; i < sizes.size(); i++) {
		const int compr = tab[idx + 1] & 0xc0;
		if (compr == 0x40 || compr == 0x80) {
			blk.resize(sizes[i]);
			if (fread(blk.data(), 1, sizes[i], f) < (size_t)sizes[i]) {
				fclose(f);
				throw std::runtime_error(
					"truncated .spg: block " + std::to_string(i) +
					" payload is short");
			}
			int dec = (compr == 0x40) ? checkMlz(blk.data(), blk.size())
						: checkHrs(blk.data(), blk.size());
			if (dec < 0) {
				fclose(f);
				throw std::runtime_error(
					"bad .spg: compressed block " + std::to_string(i) +
					" cannot be unpacked inside the loader's buffers "
					"(the core decoder would read or write out of bounds)");
			}
		} else {
			fseek(f, sizes[i], SEEK_CUR);
		}
		if (tab[idx] & 0x80) break;
		idx += 3;
	}
	fclose(f);
}

// ------------------------------------------------------------ .td0 / .dsk
//
// doTD0() reads its stream through getBytes, which reports success even at
// EOF - so the track loop that only stops on nsec==0xff spins forever on a
// truncated file - and the sector decompressors write into a 6144-byte buffer with
// no bound anywhere: btype 1 takes a 16-bit repeat count (up to 128K out),
// btype 2 a byte-sized power and a byte-sized repeat (up to ~2MB out). A
// malformed .td0 is therefore a hang or a smash, not an error. Walk the same
// structure with real bounds first. The compressed ("td") variant is inflated
// with the same lh1 decoder the core links, then walked identically.
// Returns the smallest trklen every track fits into: diskFormTrack needs
// (trklen - sum(128<<secz))/formed - 72 >= 10 and silently keeps the old
// track otherwise - see checkFdiFile.
static long checkTd0Stream(const unsigned char* img, size_t size, bool haveRem) {
	size_t pos = 0;
	auto take = [&](size_t n) -> bool {
		if (pos + n > size) return false;
		pos += n;
		return true;
	};
	auto rd = [&](size_t n) -> int {	// bounds already proven by take()
		int v = 0;
		for (size_t i = 0; i < n; i++) v |= img[pos - n + i] << (8 * i);
		return v;
	};
	// comment block (flag & 0x80): 10-byte header + len bytes
	if (haveRem) {
		if (!take(4)) throw std::runtime_error("truncated .td0: comment header");
		const int len = rd(2);
		if (!take(6 + len)) throw std::runtime_error("truncated .td0: comment body");
	}
	long minTrkLen = 0;
	int tracks = 0;
	while (tracks++ < 512) {	// no real disk has 512 tracks: a stopper
		// the terminator is a single byte upstream: an fread that gets only
		// nsec==0xff still ends the walk, so a lone 0xff is a valid end mark
		if (!take(1)) throw std::runtime_error("truncated .td0: no end marker");
		const int nsec = img[pos - 1];
		if (nsec == 0xff) return minTrkLen;	// the real end marker
		if (!take(3)) throw std::runtime_error("truncated .td0: track header");
		const int trk = img[pos - 3], head = img[pos - 2];	// +crc at pos-1
		if (trk > 127 || head > 1)
			throw std::runtime_error("bad .td0: track index " +
				std::to_string((trk << 1) + head) +
				" overruns the floppy image (255)");
		// the sectors that actually reach diskFormTrack: empty ones count,
		// size-marked ones are skipped by the core. Zero reaching it means a
		// division by scount==0 upstream, not just an empty track
		int formed = 0;
		long dataBytes = 0;	// what diskFormTrack counts: 128 << secz
		for (int i = 0; i < nsec; i++) {
			if (!take(6)) throw std::runtime_error("truncated .td0: sector header");
			const int secz = img[pos - 3], ctrl = img[pos - 2];
			if (ctrl & 0x10) {
				// an empty sector is still counted and its size code still
				// reaches diskFormTrack, where 128 << secz is a raw shift:
				// 128 << 23 makes the GAP3 arithmetic wrap positive and the
				// track memset lands megabytes past the floppy buffer
				if (secz & 0xf8)
					throw std::runtime_error("bad .td0: empty sector's "
						"size code " + std::to_string(secz) +
						" is not a size (the core shifts 128 by it)");
				formed++; dataBytes += 128l << secz; continue;
			}
			if (!take(2)) throw std::runtime_error("truncated .td0: data length");
			int dlen = rd(2);
			if (secz & 0xf8) {			// the core skips the data
				if (!take(dlen)) throw std::runtime_error("truncated .td0: sector data");
				continue;
			}
			if (dlen < 1) throw std::runtime_error("bad .td0: zero data length");
			if (!take(1)) throw std::runtime_error("truncated .td0: block type");
			const int btype = img[pos - 1];
			dlen--;
			switch (btype) {
				case 0:
					// raw bytes into a 6144-byte Sector::data
					if (dlen > 0x1800)
						throw std::runtime_error("bad .td0: raw sector is " +
							std::to_string(dlen) +
							" bytes, the buffer is 6144");
					if (!take(dlen)) throw std::runtime_error("truncated .td0: raw data");
					break;
				case 1: {
					// u16 count of u16 values -> cnt*2 bytes into 6144
					if (!take(4)) throw std::runtime_error("truncated .td0: rle pair");
					const int cnt = img[pos - 4] | (img[pos - 3] << 8);
					if (cnt > 0xc00)
						throw std::runtime_error("bad .td0: rle pair count " +
							std::to_string(cnt) +
							" overflows the 6144-byte sector");
					break;
				}
				case 2: {
					// chunks until dlen runs out: literal runs and
					// pattern repeats, pattern buffer is 8K, output 6144
					int out = 0;
					while (dlen > 0) {
						if (!take(2)) throw std::runtime_error("truncated .td0: rle chunk");
						const int ch1 = img[pos - 2], ch2 = img[pos - 1];
						dlen -= 2;
						if (ch1 == 0) {
							if (!take(ch2)) throw std::runtime_error("truncated .td0: literal run");
							dlen -= ch2;
							out += ch2;
						} else {
							const int icnt = (ch1 < 31) ? (1 << ch1) : 0x80000000;
							if (icnt > 0x2000)
								throw std::runtime_error("bad .td0: rle pattern is " +
									std::to_string(icnt) +
									" bytes, the buffer is 8192");
							if (!take(icnt)) throw std::runtime_error("truncated .td0: rle pattern");
							dlen -= icnt;
							out += icnt * ch2;
						}
						if (dlen < 0)
							throw std::runtime_error("bad .td0: rle chunk overruns its length");
						if (out > 0x1800)
							throw std::runtime_error("bad .td0: rle expands past the "
								"6144-byte sector");
					}
					break;
				}
				default:
					throw std::runtime_error("bad .td0: unknown block type " +
								 std::to_string(btype));
			}
			formed++;
			dataBytes += 128l << secz;
		}
		if (!formed)
			throw std::runtime_error("bad .td0: track declares no usable "
						 "sectors (the core divides by that count)");
		// (trklen - dataBytes)/formed - 72 >= 10 or the core skips the
		// track silently and still reports the disk loaded
		const long need = dataBytes + 82l * formed;
		if (need > minTrkLen) minTrkLen = need;
	}
	throw std::runtime_error("bad .td0: more than 512 tracks");
}

static long checkTd0File(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	unsigned char hd[12];
	const size_t got = fread(hd, 1, sizeof(hd), f);
	// a .td0 the signature refuses is a .td0 that cannot load - report it
	// here, before machineForFile switches the machine for a file that will
	// then fail anyway and leave the session wiped
	if (got < 2 || (memcmp(hd, "td", 2) != 0 && memcmp(hd, "TD", 2) != 0)) {
		fclose(f);
		throw std::runtime_error("bad .td0: no TD0 signature");
	}
	if (got < sizeof(hd)) {
		fclose(f);
		throw std::runtime_error("truncated .td0: header is " +
					 std::to_string(got) + " of 12 bytes (the core's "
					 "stream reader reports EOF as success to a track "
					 "loop that never ends)");
	}
	// the same early-outs the loader takes - turned into errors for the same
	// reason as the signature: the loader will refuse these too, just one
	// machine switch and one wiped RAM later
	const bool packed = hd[0] == 't';
	const bool good = (hd[5] == 0) && (hd[9] <= 2) && (hd[4] >= 0x14);
	if (!good) {
		fclose(f);
		throw std::runtime_error("bad .td0: density/side/version fields "
					 "the loader refuses (dens " +
					 std::to_string(hd[5]) + ", sides " +
					 std::to_string(hd[9]) + ", ver " +
					 std::to_string(hd[4]) + ")");
	}
	std::vector<unsigned char> img;
	if (packed) {
		// lh1 stream starting right after the header - the decoder the
		// core links, pointed at the same offset
		LHADecoderType* type = lha_decoder_for_name((char*)"-lh1-");
		if (!type) {
			fclose(f);
			throw std::runtime_error("bad .td0: the core's lh1 decoder is unavailable");
		}
		LHADecoder* dec = lha_decoder_new(type, [](void* buf, size_t len, void* data) -> size_t {
				FILE* fp = (FILE*)data;
				if (feof(fp)) return (size_t)0;
				if (!buf) { fseek(fp, len, SEEK_CUR); return len; }
				return fread(buf, 1, len, fp);	// honest: short reads stay short
			}, f, -1);
		if (!dec) {
			fclose(f);
			throw std::runtime_error("bad .td0: cannot start the lh1 decoder");
		}
		unsigned char chunk[8192];
		size_t n;
		// a floppy image cannot pass 8MB; the cap stops a decompression
		// bomb the way the stream bounds stop an over-read
		while (img.size() < (size_t)8 * 1024 * 1024 &&
		       (n = lha_decoder_read(dec, chunk, sizeof(chunk))) > 0)
			img.insert(img.end(), chunk, chunk + n);
		lha_decoder_free(dec);
	} else {
		fseek(f, 0, SEEK_END);
		long rest = ftell(f) - 12;
		if (rest > 0) {
			img.resize((size_t)rest);
			fseek(f, 12, SEEK_SET);
			img.resize(fread(img.data(), 1, img.size(), f));
		}
	}
	fclose(f);
	if (img.empty()) throw std::runtime_error("truncated .td0: no track data");
	return checkTd0Stream(img.data(), img.size(), (hd[7] & 0x80) != 0);
}

// loadDSK() believes every number the file declares: the track table's
// entry count is multiplied out without a limit (track indexes then run
// past the floppy's 256 slots), a sector count past 29 writes outside the
// track-info buffer, and an extended-format sector length is fread into a
// 6K data buffer at up to 64K. Walk the same layout with the file size in
// hand. Returns the smallest trklen every track fits into, like checkFdiFile:
// diskFormTrack needs (trklen - sum(128<<sz))/count - 72 >= 10 and silently
// keeps the old track otherwise.
//
// An extended-format sector may carry fewer bytes than its size code: copy
// protections store them that way. loadDSK freads what is there into a
// buffer reused from track to track and formats the full 128 << (sz & 3), so
// the tail is the previous track's sector (stack garbage on the first). Such
// tracks are listed in `shorts` for fixupDskShort to blank after the load.
struct DskShortTrack {
	int tr;				// the core's track index
	std::vector<int> sz;		// every sector's size code, in track order
	std::vector<int> slen;		// and the bytes the file gave it
};

static long checkDskFile(const std::string& path, std::vector<DskShortTrack>* shorts = nullptr) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	unsigned char head[256];
	const size_t got = fread(head, 1, sizeof(head), f);
	// a .dsk the signature refuses cannot load either - say so now, before
	// the machine is switched for it
	if (got < 8 || (memcmp(head, "EXTENDED", 8) != 0 && memcmp(head, "MV - CPC", 8) != 0)) {
		fclose(f);
		throw std::runtime_error("bad .dsk: no DSK signature");
	}
	if (got < sizeof(head)) {
		fclose(f);
		throw std::runtime_error("truncated .dsk: info block is " +
					 std::to_string(got) + " of 256 bytes");
	}
	const bool ext = memcmp(head, "EXTENDED", 8) == 0;
	fseek(f, 0, SEEK_END);
	const long fsize = ftell(f);
	const int trkcnt = head[0x30] & 0xff;
	const int sidcnt = head[0x31] & 0xff;
	const int tlen = (head[0x32] & 0xff) | ((head[0x33] & 0xff) << 8);
	// track indexes reach trkcnt*sidcnt (or 2*trkcnt single-sided) and must
	// stay inside data[256]; the extended length table is sigBuf[0x34 + i],
	// so more than 204 entries reads past the header buffer besides
	if (trkcnt * sidcnt > 256 || (sidcnt == 1 && trkcnt > 128) ||
	    (ext && trkcnt * sidcnt > 204)) {
		fclose(f);
		throw std::runtime_error("bad .dsk: " + std::to_string(trkcnt) + " tracks x " +
					 std::to_string(sidcnt) + " sides overruns the floppy image");
	}
	long pos = 0x100;
	long minTrkLen = 0;
	// the track loadDSK formats: one per entry, two per entry single-sided,
	// whether the entry has a track behind it or not
	for (int i = 0, tr = 0; i < trkcnt * sidcnt; i++, tr += (sidcnt == 1) ? 2 : 1) {
		// the core reads the length byte through a plain char, signed on
		// most hosts: 0x80 and up turns negative there, the track is skipped
		// without the file position moving, and every later track is read
		// from where this one starts - and on an unsigned-char host it is a
		// 32K+ track instead. No real track is that long; a walk that depends
		// on the host is not one to vouch for
		if (ext && head[0x34 + i] >= 0x80) {
			fclose(f);
			throw std::runtime_error("bad .dsk: track " + std::to_string(i) +
						 " declares " + std::to_string(head[0x34 + i] << 8) +
						 " bytes, no real track size (the core reads the "
						 "entry signed and walks the image differently by host)");
		}
		const int rtlen = ext ? (head[0x34 + i] << 8) : tlen;
		if (rtlen <= 0) continue;
		if (pos + 256 > fsize) {
			fclose(f);
			throw std::runtime_error("truncated .dsk: track " + std::to_string(i) +
						 " header needs 256 bytes at " + std::to_string(pos));
		}
		fseek(f, pos, SEEK_SET);
		unsigned char tib[256];
		if (fread(tib, 1, sizeof(tib), f) < sizeof(tib)) {
			fclose(f);
			throw std::runtime_error("truncated .dsk: track " + std::to_string(i) +
						 " header read short at " + std::to_string(pos));
		}
		// the disk signature already claimed this is a .dsk - a track block
		// that is not "Track-Info" is a broken disk, not another format, and
		// reporting it here keeps the error ahead of any machine switch
		if (memcmp(tib, "Track-Info\r\n", 12) != 0) {
			fclose(f);
			throw std::runtime_error("bad .dsk: track " + std::to_string(i) +
						 " has no track-info signature");
		}
		// TrackInfBlock starts at +12: 4 spare + track + side + dataRate +
		// recMode + secSize, then secCount at +21; sector infos at +24
		const int secCount = tib[0x15];
		if (secCount == 0) {
			fclose(f);
			throw std::runtime_error("bad .dsk: track " + std::to_string(i) +
						 " declares no sectors (the core divides by "
						 "that count forming the track)");
		}
		if (secCount > 29) {
			fclose(f);
			throw std::runtime_error("bad .dsk: " + std::to_string(secCount) +
						 " sectors overrun the track-info table (29)");
		}
		long dataNeed = 0;
		long dataBytes = 0;	// what diskFormTrack counts: 128 << sz
		DskShortTrack layout{tr, {}, {}};
		bool hasShort = false;
		for (int s = 0; s < secCount; s++) {
			const unsigned char* sib = tib + 0x18 + s * 8;
			// sib[3] is copied verbatim into Sector.sz, and diskFormTrack
			// shifts 128 by it for the GAP3 math - a code over 7 (the core
			// masks with & 7 elsewhere) turns the per-sector gap into a
			// runaway past the floppy's track buffer
			if (sib[3] > 7) {
				fclose(f);
				throw std::runtime_error("bad .dsk: sector " + std::to_string(s) +
					"'s size code " + std::to_string(sib[3]) +
					" is not a size (the core shifts 128 by it)");
			}
			int slen;
			if (!ext) slen = (sib[3] > 5) ? 0x1800 : (0x80 << sib[3]) & 0xffff;
			else      slen = sib[6] | (sib[7] << 8);
			if (slen > 0x1800) {
				fclose(f);
				throw std::runtime_error("bad .dsk: sector " + std::to_string(s) +
					" declares " + std::to_string(slen) +
					" bytes, the core's buffer is 6144");
			}
			// the core writes 128<<(sz&3) bytes of sector data into the
			// track whatever slen was read - a shorter fread leaves the
			// rest holding the previous track's bytes, blanked after
			// the load
			if (slen < (0x80 << (sib[3] & 3))) hasShort = true;
			layout.sz.push_back(sib[3]);
			layout.slen.push_back(slen);
			dataNeed += slen;
			dataBytes += 128l << sib[3];
		}
		if (pos + 256 + dataNeed > fsize) {
			fclose(f);
			throw std::runtime_error("truncated .dsk: track " + std::to_string(i) +
						 " sectors run past the file end");
		}
		if (hasShort && shorts) shorts->push_back(std::move(layout));
		const long need = dataBytes + 82l * secCount;
		if (need > minTrkLen) minTrkLen = need;
		pos += rtlen;
	}
	fclose(f);
	return minTrkLen;
}

// The short sectors' tails, blanked where diskFormTrack put them. They are
// found by position, the layout diskFormTrack lays down, rather than by
// sector number: a protected track is exactly where two sectors share one,
// and the core's sector map only remembers the last. The CRC is rewritten the
// way diskPutSectorData does it, or the zeros would read back as a CRC error.
static void fixupDskShort(Floppy* flp, const std::vector<DskShortTrack>& tracks) {
	for (const DskShortTrack& t : tracks) {
		const int n = (int)t.sz.size();
		if (t.tr > 255 || n == 0) continue;
		long sum = 0;
		for (int sz : t.sz) sum += 128l << sz;
		const long gap = (flp->trklen - sum) / n - 72;	// diskFormTrack's GAP3
		if (gap < 10) continue;				// it left this track alone
		unsigned char* trk = flp->data[t.tr].byte;
		// 12 zeros + index mark, then per sector: GAP1, sync, ID mark, CHRN,
		// CRC, GAP2, sync, data mark (70 bytes), the data, its CRC, GAP3
		long pos = 16;
		for (int k = 0; k < n; k++) {
			const int ln = 128 << (t.sz[k] & 3);
			const long data = pos + 70;
			if (data + ln + 2 > flp->trklen) break;
			if (t.slen[k] < ln) {
				memset(trk + data + t.slen[k], 0, ln - t.slen[k]);
				const unsigned short crc = getCrc(trk + data - 4, ln + 4);
				trk[data + ln] = crc >> 8;
				trk[data + ln + 1] = crc & 0xff;
			}
			pos = data + ln + 2 + gap;
		}
	}
}

// .fdi - loadFDI copies every sector's raw size code into Sector.sz, which
// diskFormTrack shifts by unchecked (128 << sz - a 23 wraps the gap math
// positive and the track memset walks off the floppy's buffer). The track
// index (cyl << 1) + head has the same problem upstream's `tr > 255` guard
// comes one line too late for: flp->data[tr] is read first. Walk the format
// here and refuse what the core cannot survive.
// Returns the smallest trklen every declared track fits into: the core's
// diskFormTrack needs (trklen - sum(128<<sz))/nsec - 72 >= 10 and silently
// leaves the track's old contents when it fails, while reporting success.
// The caller compares the result against the actual drive's trklen.
static long checkFdiFile(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	unsigned char hd[14];
	if (fread(hd, 1, sizeof(hd), f) < sizeof(hd) || memcmp(hd, "FDI", 3) != 0) {
		fclose(f);
		throw std::runtime_error("bad .fdi: no FDI signature");
	}
	fseek(f, 0, SEEK_END);
	const long fsize = ftell(f);
	auto rd16 = [](const unsigned char* p) { return p[0] | (p[1] << 8); };
	const int cyls = rd16(hd + 4), heads = rd16(hd + 6);
	const int dData = rd16(hd + 10), xSize = rd16(hd + 12);
	if (heads < 1 || heads > 2) {		// the loader's own refusal
		fclose(f);
		throw std::runtime_error("bad .fdi: " + std::to_string(heads) + " heads");
	}
	// (cyl << 1) + head indexes flp->data[256], read before the tr bound
	if ((cyls - 1) * 2 + (heads - 1) > 255) {
		fclose(f);
		throw std::runtime_error("bad .fdi: " + std::to_string(cyls) + " cyls x " +
					 std::to_string(heads) + " sides overruns the floppy image");
	}
	long pos = sizeof(hd) + xSize;		// the comment field the loader skips
	long minTrkLen = 0;
	for (int cyl = 0; cyl < cyls; cyl++) {
		for (int head = 0; head < heads; head++) {
			if (pos + 7 > fsize) {
				fclose(f);
				throw std::runtime_error("truncated .fdi: track header");
			}
			fseek(f, pos, SEEK_SET);
			unsigned char thd[7];
			if (fread(thd, 1, sizeof(thd), f) < sizeof(thd)) {
				fclose(f);
				throw std::runtime_error("truncated .fdi: track header");
			}
			const long toff = (long)thd[0] | (thd[1] << 8) | (thd[2] << 16) |
					  ((long)thd[3] << 24);
			const int nsec = thd[6];
			if (nsec == 0) {	// (trklen - 0) / 0 in diskFormTrack
				fclose(f);
				throw std::runtime_error("bad .fdi: track " +
						std::to_string(cyl) + "/" + std::to_string(head) +
						" declares 0 sectors (the core divides by "
						"the count)");
			}
			pos += 7;
			long dataBytes = 0;
			for (int s = 0; s < nsec; s++) {
				if (pos + 7 > fsize) {
					fclose(f);
					throw std::runtime_error("truncated .fdi: sector header");
				}
				unsigned char shd[7];
				if (fread(shd, 1, sizeof(shd), f) < sizeof(shd)) {
					fclose(f);
					throw std::runtime_error("truncated .fdi: sector header");
				}
				pos += 7;
				if (shd[3] > 7) {		// Sector.sz verbatim -> 128 << sz
					fclose(f);
					throw std::runtime_error("bad .fdi: sector's size code " +
						std::to_string(shd[3]) +
						" is not a size (the core shifts 128 by it)");
				}
				dataBytes += 128l << shd[3];	// what diskFormTrack counts
				const long soff = dData + toff + rd16(shd + 5);
				const int slen = 128 << (shd[3] & 3);
				if (soff < 0 || soff + slen > fsize) {
					fclose(f);
					throw std::runtime_error("truncated .fdi: sector data "
						"at " + std::to_string(soff) + " runs past the file");
				}
			}
			const long need = dataBytes + 82l * nsec;	// dsz >= 10 upstream
			if (need > minTrkLen) minTrkLen = need;
		}
	}
	fclose(f);
	return minTrkLen;
}

// .udi - loadUDI loops i < maxTrk+1 and forms rt = (i << 1) + side into the
// same 256-entry floppy table; a 129-track image writes track 256.
// Each track record is self-delimiting, so a walk also proves the file's
// declared lengths fit inside it instead of trusting the core's freads.
static void checkUdiFile(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	unsigned char hd[16];
	if (fread(hd, 1, sizeof(hd), f) < sizeof(hd) ||
	    memcmp(hd, "UDI!", 4) != 0 || hd[8] != 0) {
		fclose(f);
		throw std::runtime_error("bad .udi: no UDI signature");
	}
	fseek(f, 0, SEEK_END);
	const long fsize = ftell(f);
	const int maxTrk = hd[9], dbSide = hd[10] ? 1 : 0;
	if (maxTrk * 2 + dbSide > 255) {
		fclose(f);
		throw std::runtime_error("bad .udi: " + std::to_string(maxTrk + 1) +
					 " cyls" + (dbSide ? " x 2 sides" : "") +
					 " overruns the floppy image (256 tracks)");
	}
	long pos = 16;
	auto room = [&](long n) -> bool { return pos + n <= fsize; };
	auto readLen = [&](int bytes) -> long {
		unsigned char b[4] = {0, 0, 0, 0};
		for (int i = 0; i < bytes; i++) {
			const int c = fgetc(f);
			if (c == EOF) return -1;
			b[i] = (unsigned char)c;
		}
		long v = 0;
		for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | b[i];
		return v;
	};
	const int recs = (maxTrk + 1) * (dbSide ? 2 : 1);
	for (int i = 0; i < recs; i++) {
		if (pos + 1 > fsize) {
			fclose(f);
			throw std::runtime_error("truncated .udi: track record " +
						 std::to_string(i));
		}
		fseek(f, pos, SEEK_SET);
		const int type = fgetc(f);
		pos += 1;
		long len;
		if (type == 0) {			// a real track: len + data + bitfield
			len = readLen(2);
			if (len < 0 || !room(2 + len + (len + 7) / 8)) {
				fclose(f);
				throw std::runtime_error("truncated .udi: track data " +
							 std::to_string(i));
			}
			pos += 2 + len + (len + 7) / 8;
		} else {				// unknown format: len + skip
			// loadUDI mallocs a track buffer for every record and frees
			// it only on type 0 - the rest leaks, and the data is never
			// used even when it survives, so there is nothing to keep
			fclose(f);
			throw std::runtime_error("bad .udi: track record " +
						 std::to_string(i) + " declares unknown format " +
						 std::to_string(type) + " (the core leaks its "
						 "buffer and never uses the data)");
		}
	}
	fclose(f);
}

// The remaining formats share one shape: the core's parser freads what the
// file says exists and reports success even when the file runs out, so a
// truncated image "loads" a disk/snapshot built out of whatever the
// destination buffers already held. These walkers index a memory image of
// the file, so every declared length is checked against the bytes that
// actually exist - before machineForFile can switch anything.
static std::vector<unsigned char> fileBytes(const std::string& path, const char* tag) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	if (n < 0) n = 0;
	rewind(f);
	std::vector<unsigned char> v((size_t)n);
	if (n > 0 && fread(v.data(), 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		throw std::runtime_error(std::string("truncated ") + tag + ": short read");
	}
	fclose(f);
	return v;
}

static unsigned rd16le(const std::vector<unsigned char>& v, size_t p) {
	return v[p] | ((unsigned)v[p + 1] << 8);
}

static unsigned rd24le(const std::vector<unsigned char>& v, size_t p) {
	return v[p] | ((unsigned)v[p + 1] << 8) | ((unsigned)v[p + 2] << 16);
}

static unsigned long rd32le(const std::vector<unsigned char>& v, size_t p) {
	return v[p] | ((unsigned long)v[p + 1] << 8) | ((unsigned long)v[p + 2] << 16) |
	       ((unsigned long)v[p + 3] << 24);
}

// loadTRD's `do { fread; flp_format_trk } while (!feof)` formats a track out
// of whatever the buffer holds when a fread comes up short - the remainder
// of a partial final track, or a whole extra track when the file ends on a
// track boundary. Neither came from the file: empty the sectors the file
// never carried, and wipe the boundary phantom, sector map included.
static void fixupTrdTail(Floppy* flp, const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fclose(f);
	if (n <= 0) return;
	const int last = (int)(n >> 12);
	if (last >= 256) return;
	if ((n & 0xfff) == 0) {		// the extra loop pass invented this track
		memset(flp->data[last].byte, 0, flp->trklen);
		memset(flp->data[last].field, 0, flp->trklen);
		memset(flp->data[last].map, 0, sizeof(flp->data[last].map));
		return;
	}
	// file size is a multiple of 256 (checkTrdFile), so the partial track's
	// sectors up to `covered` are real; the rest are the previous track's
	// leftovers - present but empty, which is what a short .trd means.
	// diskPutSectorData also rewrites the CRC - zeroed data under the old
	// bytes' checksum reads back as a CRC error on the WD1793
	unsigned char zbuf[256] = {0};
	for (int sc = ((int)(n & 0xfff) >> 8) + 1; sc <= 16; sc++)
		diskPutSectorData(flp, (unsigned char)last, (unsigned char)sc, zbuf, 256);
}

// .trd - loadTRD already refuses what it cannot serve; it just refuses one
// machine switch too late. Same size rule, asked earlier.
static void checkTrdFile(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fclose(f);
	if (n <= 0 || (n & 0xff) != 0 || n > 0xfe000)
		throw std::runtime_error("bad .trd: " + std::to_string(n) +
					 " bytes is not a multiple of 256 in 1..0xfe000 "
					 "(the loader's own refusal, asked pre-switch)");
}

// .scl - loadSCL freads the header and every 14-byte descriptor unchecked:
// a file that is only the 9-byte signature and a count loads fine with a
// catalogue fabricated from the caller's stack. The descriptors and the
// sector payload they declare must exist.
static void checkSclFile(const std::string& path) {
	std::vector<unsigned char> v = fileBytes(path, ".scl");
	if (v.size() < 9 || memcmp(v.data(), "SINCLAIR", 8) != 0)
		throw std::runtime_error("bad .scl: no SINCLAIR signature");
	const int files = v[8];
	if (files > 128)
		throw std::runtime_error("bad .scl: " + std::to_string(files) +
					 " files (the loader refuses over 128)");
	const size_t dsc = 9 + (size_t)files * 14;
	if (v.size() < dsc)
		throw std::runtime_error("truncated .scl: " + std::to_string(files) +
					 " descriptors declared, " +
					 std::to_string((v.size() > 9 ? v.size() - 9 : 0) / 14) +
					 " fit");
	long payload = 0;			// each descriptor's sector count x 256
	for (int i = 0; i < files; i++) payload += (long)v[9 + i * 14 + 13] * 256;
	if (v.size() < dsc + (size_t)payload)
		throw std::runtime_error("truncated .scl: sector bodies end at " +
					 std::to_string(dsc + (size_t)payload) +
					 " past the image's " + std::to_string(v.size()));
}

// .tap - loadTAP honors every block's declared length even when the fread
// behind it came back short, so a trailing length word manufactures a block
// out of the previous one's bytes. Walk the [len,data] chain to the end.
static void checkTapFile(const std::string& path) {
	std::vector<unsigned char> v = fileBytes(path, ".tap");
	size_t pos = 0;
	while (pos < v.size()) {
		if (pos + 2 > v.size()) break;	// a lone byte: fgetw hits EOF, skipped
		const unsigned len = rd16le(v, pos);
		pos += 2;
		if (pos + len > v.size())
			throw std::runtime_error("truncated .tap: block declares " +
						 std::to_string(len) + " bytes, " +
						 std::to_string(v.size() - pos) + " remain");
		pos += len;
	}
}

// .sna - the loader decides 48K vs 128K on size alone and freads every page
// unchecked: a 27-byte header-only file "loads" the stack garbage pageBuf
// happens to hold. A 48K needs exactly 27+3*16K. The 128K file is 27 + 3
// pages + a 4-byte block (PC, port 7ffd, trdos) + the remaining banks: when
// the paged-in bank (byte 49181) is 2 or 5 the bank loop skips nothing and
// six more pages follow - 147487 bytes - otherwise one page is skipped and
// five follow - 131103. Anything short fabricates banks from pageBuf.
static void checkSnaFile(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	if (n < 49179) {
		fclose(f);
		throw std::runtime_error("truncated .sna: " + std::to_string(n) +
					 " bytes cannot hold a 48K image (49179)");
	}
	if (n == 49179) { fclose(f); return; }
	int port = 0;
	if (fseek(f, 49181, SEEK_SET) != 0 || fread(&port, 1, 1, f) != 1)
		port = -1;
	fclose(f);
	const long need = ((port & 7) == 2 || (port & 7) == 5) ? 147487 : 131103;
	if (n < need)
		throw std::runtime_error("truncated .sna: " + std::to_string(n) +
					 " bytes cannot hold this 128K image (" +
					 std::to_string(need) + ")");
}

// z80uncompress() mirrored as a byte count: same stops - the 00 ed ed 00
// terminator, the input running out, or maxlen reached. Returns what the
// core would have produced; a snapshot page that produces less than a full
// 16K loads the rest of the destination buffer as if it were data.
static long z80InflatedSize(const unsigned char* p, size_t n, long maxlen) {
	long out = 0;
	size_t i = 0;
	unsigned char lst = 0xed;
	while (i < n && out < maxlen) {
		unsigned char tmp = p[i++];
		if (tmp == 0xed) {
			if (i >= n) break;
			tmp = p[i++];
			if (tmp == 0xed) {
				if (i >= n) break;
				unsigned char cnt = p[i++];
				if (lst == 0 && cnt == 0) break;	// terminator
				if (i >= n) break;
				tmp = p[i++];
				// upstream's do{emit; cnt--}while(cnt && room): cnt 0 wraps
				// to 255, so a zero count is 256 emissions, not none
				long emit = cnt ? cnt : 256;
				const long room = maxlen - out;
				out += emit < room ? emit : room;
			} else {
				out++;
				if (out < maxlen) out++;
				lst = tmp;
			}
		} else {
			out++;
			lst = tmp;
		}
	}
	return out;
}

// z80_hardware() verbatim: the byte-34 value by header version, with byte
// 37.7 folded in. Returns the SNAP_HW_* the core would pick.
static int z80SnapHw(bool v3, int hw, bool mod) {
	switch (hw) {
		case 0:
		case 1:
		case 2: return SNAP_HW_48K;
		case 3: if (v3) return SNAP_HW_48K;
			return mod ? SNAP_HW_PLUS2 : SNAP_HW_128K;
		case 4: return mod ? SNAP_HW_PLUS2 : SNAP_HW_128K;
		case 5:
		case 6: if (!v3) return SNAP_HW_UNKNOWN;
			return mod ? SNAP_HW_PLUS2 : SNAP_HW_128K;
		case 7:
		case 8: return mod ? SNAP_HW_PLUS2A : SNAP_HW_PLUS3;
		case 9: return SNAP_HW_PENTAGON;
		case 10: return SNAP_HW_SCORPION;
		case 12: return SNAP_HW_PLUS2;
		case 13: return SNAP_HW_PLUS2A;
	}
	return SNAP_HW_UNKNOWN;
}

static int z80HwBanks(int snap) {
	switch (snap) {
		case SNAP_HW_48K: return 3;
		case SNAP_HW_SCORPION: return 16;
		case SNAP_HW_UNKNOWN: return 0;
		default: return 8;		// 128K, +2, +2A, +3, Pentagon
	}
}

// .z80 - loadZ80 freads the 30-byte header unchecked, then either a raw 48K
// image, a compressed one, or the paged blocks - every short read lands in
// emulated RAM anyway. Walk the declared structure instead.
static void checkZ80File(const std::string& path) {
	std::vector<unsigned char> v = fileBytes(path, ".z80");
	if (v.size() < 30)
		throw std::runtime_error("truncated .z80: no 30-byte header");
	if (v[6] | v[7]) {					// v1: PC is set
		int f12 = (v[12] == 0xff) ? 1 : v[12];		// the loader's own fix
		if (f12 & 0x20) {
			if (z80InflatedSize(v.data() + 30, v.size() - 30, 0xc000) < 0xc000)
				throw std::runtime_error("truncated .z80: compressed 48K "
							 "data does not fill 49152 bytes");
		} else if (v.size() < 30 + 0xc000) {
			throw std::runtime_error("truncated .z80: 48K image is " +
						 std::to_string(v.size() - 30) + " of 49152 bytes");
		}
		return;
	}
	// v2/v3: extended header at 30, hw byte at 34, flag byte at 37; the
	// loader reads through offset 54 unconditionally and up to 86 for 1ffd
	if (v.size() < 55)
		throw std::runtime_error("truncated .z80: extended header is " +
					 std::to_string(v.size()) + " bytes");
	const unsigned hlen = rd16le(v, 30);
	if (v.size() < 32 + hlen)
		throw std::runtime_error("truncated .z80: header declares " +
					 std::to_string(hlen) + " extra bytes, " +
					 std::to_string(v.size() - 32) + " exist");
	if (hlen > 54 && v.size() < 87)
		throw std::runtime_error("truncated .z80: v3 header lacks the 1ffd byte");
	const int snap = z80SnapHw(hlen > 23, v[34], (v[37] & 0x80) != 0);
	const int banks = z80HwBanks(snap);
	if (banks == 0)
		throw std::runtime_error("bad .z80: hardware byte " +
					 std::to_string(v[34]) + " names a machine the "
					 "loader refuses (ERR_Z80_HW)");
	// blocks: [len:2][page:1][data] - len ffff is a raw 16K page; the
	// loader stops at the file end with whatever banks it found. v2 does
	// not seek: blocks start where its unconditional reads end (offset 55)
	// whatever hlen says; v3 seeks past the extended header
	size_t pos = (hlen > 23) ? 32 + hlen : 55;
	unsigned seen = 0;
	// the loader decrements its counter on every RAM block, duplicates
	// included, and stops when it hits zero - so blocks after that never
	// load. Simulate the same stop, then check every wanted bank was seen
	int btm = banks;
	while (btm > 0) {
		if (pos + 3 > v.size())
			throw std::runtime_error("truncated .z80: ran out of pages with " +
						 std::to_string(btm) + " RAM banks missing");
		const unsigned len = rd16le(v, pos);
		const int pg = v[pos + 2];
		pos += 3;
		if (len == 0xffff) {
			if (pos + 0x4000 > v.size())
				throw std::runtime_error("truncated .z80: raw page " +
							 std::to_string(pg));
			pos += 0x4000;
		} else {
			if (pos + len > v.size())
				throw std::runtime_error("truncated .z80: page " +
							 std::to_string(pg) + " declares " +
							 std::to_string(len) + " bytes");
			if (z80InflatedSize(v.data() + pos, len, 0x4000) != 0x4000)
				throw std::runtime_error("bad .z80: page " +
							 std::to_string(pg) + " inflates short");
			pos += len;
		}
		// z80_block_bank(): the 48K layout is special - page 4 is bank 2,
		// 5 is bank 0, 8 is bank 5; everyone else is page-3
		const int bnk = (snap == SNAP_HW_48K)
			? (pg == 4 ? 2 : pg == 5 ? 0 : pg == 8 ? 5 : -1)
			: ((pg > 2) && (pg - 3 < banks) ? pg - 3 : -1);
		if (bnk >= 0) { seen |= 1u << bnk; btm--; }
		else if (pg > 3 && pg != 11) break;
	}
	if (btm > 0)
		throw std::runtime_error("truncated .z80: image holds fewer RAM "
					 "banks than its hardware wants");
	const unsigned need = (snap == SNAP_HW_48K) ? ((1u << 0) | (1u << 2) | (1u << 5))
						    : ((1u << banks) - 1);
	if ((seen & need) != need)
		throw std::runtime_error("bad .z80: duplicate pages ended the load "
					 "with RAM banks still missing");
}

// .tzx - tzxBodySize() knows every block's body length; the reader trusts
// those declared sizes, so block #11 with datalen 0 mallocs(0) then freads
// (len-1) = SIZE_MAX, and block #19 walks symbol tables it never bounds
// against its own buffer. Walk the chain and apply the same sizes.
static long tzxBodySize(const std::vector<unsigned char>& v, size_t pos, int id) {
	auto need = [&](size_t off, size_t n) {
		if (pos + off + n > v.size())
			throw std::runtime_error("truncated .tzx: block " +
						 std::to_string(id) + "'s own length field "
						 "runs past the file");
	};
	switch (id) {
		case 0x10: need(2, 2); return 0x04 + rd16le(v, pos + 0x02);
		case 0x11: need(0x0f, 3); return 0x12 + rd24le(v, pos + 0x0f);
		case 0x12: return 0x04;
		case 0x13: need(0, 1); return 0x01 + v[pos] * 2;
		case 0x14: need(7, 3); return 0x0a + rd24le(v, pos + 0x07);
		case 0x15: need(5, 3); return 0x08 + rd24le(v, pos + 0x05);
		case 0x20: case 0x23: case 0x24: return 0x02;
		case 0x21: case 0x30: need(0, 1); return 0x01 + v[pos];
		case 0x22: case 0x25: case 0x27: return 0;
		case 0x26: need(0, 2); return 0x02 + rd16le(v, pos) * 2;
		case 0x28: case 0x32: need(0, 2); return 0x02 + rd16le(v, pos);
		case 0x2a: return 0x04;
		case 0x2b: return 0x05;
		case 0x31: need(1, 1); return 0x02 + v[pos + 0x01];
		case 0x33: need(0, 1); return 0x01 + v[pos] * 3;
		case 0x34: return 0x08;
		case 0x35: need(0x10, 4); return 0x14 + (long)rd32le(v, pos + 0x10);
		case 0x40: need(1, 3); return 0x04 + rd24le(v, pos + 0x01);
		case 0x5a: return 0x09;
	}
	need(0, 4);
	return 0x04 + (long)rd32le(v, pos);	// newer ids lead with their length
}

// The reader turns the tape into pulses, a TapeSignal (8 bytes) each, in
// buffers it grows by realloc without checking and counts with ints - and a
// file of a few bytes asks for tens of billions: #12 is 65535 pulses in four
// bytes, a #24 loop plays it 65535 times, calls and jumps multiply again, #19
// with a one-symbol alphabet emits totd symbols without reading any data, and
// #18 inflates its CSW stream with no limit. So the interpreter runs here
// first, counting instead of emitting. 2^26 pulses is 512 MiB of TapeSignal
// (and the biggest block is held twice for a moment while tap_add_block copies
// it); a 48K program at standard speed is under a million pulses and an hour
// of turbo data some 20M, so a real tape stays well inside.
const long long kTzxMaxPulses = 1LL << 26;

[[noreturn]] static void tzxTooLong() {
	throw std::runtime_error("bad .tzx: the tape plays out to more than " +
				 std::to_string(kTzxMaxPulses) + " pulses (" +
				 std::to_string((kTzxMaxPulses * 8) >> 20) +
				 " MiB of signal the core would allocate unchecked)");
}

// #19 symbol: pulses up to the first zero length, at most n of them. The
// first one may merge into the previous pulse instead; counting it anyway
// keeps this an upper bound.
static int tzxSymPulses(const unsigned char* def, int n) {
	int i = 0;
	while (i < n && (def[1 + i * 2] | def[2 + i * 2]) != 0) i++;
	return i;
}

// a CSW stream (tzxBlock18): a byte per pulse, a zero byte takes the four after
// it as the length - and a zero with less than four behind it ends the stream
struct CswCount {
	long long pulses = 0;
	int owed = -1;
	void feed(const unsigned char* b, size_t n) {
		for (size_t i = 0; i < n; i++) {
			if (owed > 0) {
				if (--owed == 0) { pulses++; owed = -1; }
			} else if (b[i] == 0) {
				owed = 4;
			} else {
				pulses++;
			}
		}
	}
};

// The pulses one block adds, as the core's block readers add them; a pause is
// one pulse whatever its length (the readers skip a zero one, so this is at
// most one high). Bodies are already proven inside the file.
static long long tzxBlockPulses(const std::vector<unsigned char>& v, size_t p, int id) {
	switch (id) {
		case 0x10: {		// a standard block: pilot, two syncs, 16 per byte
			const long len = rd16le(v, p + 2);
			const int pdur = (len > 0 && (v[p + 4] & 0x80)) ? 3223 : 8063;
			return pdur + 2 + 16LL * len + 1;
		}
		case 0x11: {
			const long dlen = rd24le(v, p + 0x0f);
			const int bits = std::min<int>(v[p + 0x0c], 8);
			return rd16le(v, p + 0x0a) + 2 + 16LL * (dlen - 1) + 2 * bits + 1;
		}
		case 0x12: return rd16le(v, p + 2);
		case 0x13: return v[p];
		case 0x14: {
			const long dlen = rd24le(v, p + 7);
			const int bits = std::min<int>(v[p + 4], 8);
			return 16LL * (dlen - 1) + 2 * bits + 1;
		}
		case 0x15: {		// direct recording: a pulse per level run
			int last = v[p + 4];
			if (last < 1 || last > 8) last = 8;
			const long bits = ((long)rd24le(v, p + 5) - 1) * 8 + last;
			if (bits <= 0) return 1;
			const unsigned char* d = v.data() + p + 8;
			long long runs = 1;
			int prev = d[0] >> 7;
			for (long i = 1; i < bits; i++) {
				const int b = (d[i >> 3] >> (7 - (i & 7))) & 1;
				if (b != prev) { runs++; prev = b; }
			}
			return runs + 1;
		}
		case 0x18: {
			const int len = (int)rd32le(v, p) - 10;
			if (len < 1 || rd24le(v, p + 6) < 1) return 0;	// read, nothing added
			const unsigned char* d = v.data() + p + 14;
			CswCount cnt;
			if (v[p + 9] != 2) {
				cnt.feed(d, (size_t)len);
			} else {
				// the core inflates the whole stream into memory before it
				// reads a pulse out of it, growing the buffer with no cap;
				// one byte per pulse is what CSW spends on a real tape, so
				// the pulse cap in bytes bounds the stream too
				z_stream zs;
				memset(&zs, 0, sizeof(zs));
				if (inflateInit(&zs) == Z_OK) {
					unsigned char out[65536];
					zs.next_in = const_cast<unsigned char*>(d);
					zs.avail_in = (uInt)len;
					int err = Z_OK;
					while (err == Z_OK) {
						zs.next_out = out;
						zs.avail_out = sizeof(out);
						err = inflate(&zs, Z_NO_FLUSH);
						cnt.feed(out, sizeof(out) - zs.avail_out);
						if ((long long)zs.total_out > kTzxMaxPulses) {
							inflateEnd(&zs);
							throw std::runtime_error("bad .tzx: a block 0x18 CSW stream "
								"inflates past " + std::to_string(kTzxMaxPulses) +
								" bytes (the core inflates it into memory unbounded)");
						}
					}
					inflateEnd(&zs);
				}
			}
			return cnt.pulses + 1;
		}
		case 0x19: {
			const int len = (int)rd32le(v, p) - 14;
			if (len < 1) return 0;
			const long totp = (int)rd32le(v, p + 6), totd = (int)rd32le(v, p + 12);
			const int npp = v[p + 10], npd = v[p + 16];
			const int asp = v[p + 11] ? v[p + 11] : 256, asd = v[p + 17] ? v[p + 17] : 256;
			const int psz = 1 + npp * 2, dsz = 1 + npd * 2;
			const unsigned char* buf = v.data() + p + 18;
			long long pulses = 1;
			long ptr = 0;
			if (totp > 0) {		// the table bounds were proven by the walk
				ptr = (long)asp * psz;
				for (long i = 0; i < totp && ptr + 3 <= len; i++, ptr += 3) {
					const int s = buf[ptr], cnt = buf[ptr + 1] | (buf[ptr + 2] << 8);
					if (s < asp) pulses += (long long)cnt * tzxSymPulses(buf + s * psz, npp);
				}
			}
			if (totd > 0) {
				const unsigned char* sym = buf + ptr;
				ptr += (long)asd * dsz;
				int nb = 0;
				while ((1 << nb) < asd) nb++;
				if (nb == 0) {
					// a one-symbol alphabet takes no bits: the core's loop
					// emits all totd symbols and never runs out of data
					pulses += (long long)totd * tzxSymPulses(sym, npd);
				} else {
					const long long avail = (ptr < len) ? (long long)(len - ptr) * 8 : 0;
					const long long n = std::min<long long>(totd, avail / nb);
					const unsigned char* d = buf + ptr;
					long long bit = 0;
					for (long long i = 0; i < n && pulses <= kTzxMaxPulses; i++) {
						int s = 0;
						for (int k = 0; k < nb; k++, bit++)
							s = (s << 1) | ((d[bit >> 3] >> (7 - (bit & 7))) & 1);
						if (s < asd) pulses += tzxSymPulses(sym + s * dsz, npd);
					}
				}
			}
			return pulses;
		}
		case 0x20: return 1;
	}
	return 0;
}

static void checkTzxFile(const std::string& path) {
	std::vector<unsigned char> v = fileBytes(path, ".tzx");
	if (v.size() < 10 || memcmp(v.data(), "ZXTape!", 7) != 0 || v[7] != 0x1a)
		throw std::runtime_error("bad .tzx: no ZXTape! signature");
	struct Blk { int id; size_t body; };
	std::vector<Blk> blocks;
	size_t pos = 10;
	while (pos < v.size()) {
		const int id = v[pos++];
		const size_t body = pos;
		blocks.push_back({id, body});
		const long bsz = tzxBodySize(v, body, id);
		if (bsz < 0 || body + (size_t)bsz > v.size())
			throw std::runtime_error("truncated .tzx: block " +
						 std::to_string(id) + " declares " +
						 std::to_string(bsz) + " bytes, " +
						 std::to_string(v.size() - body) + " remain");
		if (id == 0x11 || id == 0x14) {	// fread(len - 1) + a last byte
			const unsigned dlen = rd24le(v, body + (id == 0x11 ? 0x0f : 0x07));
			if (dlen < 1)
				throw std::runtime_error("bad .tzx: block " +
							 std::to_string(id) +
							 " declares a 0-length data field (the"
							 " core still reads one byte)");
		} else if (id == 0x19) {
			// body: len:4, pause:2, totp:4, npp:1, asp:1, totd:4,
			// npd:1, asd:1, then pilot table, pilot runs, data
			// table, data bits - the two tables are read at indexes
			// bounded only by the alphabet sizes, never by the block
			const unsigned long blen = rd32le(v, body);
			if (blen >= 15) {	// len<1 upstream returns early
				const long bufLen = (long)blen - 14;
				const long totp = (long)rd32le(v, body + 6);
				const int npp = v[body + 10], asp = v[body + 11];
				const long totd = (long)rd32le(v, body + 12);
				const int npd = v[body + 16], asd = v[body + 17];
				const long psz = 1 + npp * 2, dsz2 = 1 + npd * 2;
				const long aps = asp ? asp : 256, ads = asd ? asd : 256;
				long ptr = 0;
				if (totp > 0) {
					if (aps * psz > bufLen)
						throw std::runtime_error("bad .tzx: block 0x19 "
							 "pilot table overruns its block");
					ptr = aps * psz;
					for (long i = 0; i < totp && ptr + 3 <= bufLen;
					     i++, ptr += 3) {}
				}
				if (totd > 0 && ptr + ads * dsz2 > bufLen)
					throw std::runtime_error("bad .tzx: block 0x19 "
						 "data table overruns its block");
			}
		}
		pos = body + (size_t)bsz;
	}
	// loadTZX's interpreter: the same program counter, loop, call and jump
	// state, and the same step limit, so the pulses counted are the pulses
	// the core will emit. A block's count does not change between visits.
	const int cnt = (int)blocks.size();
	std::vector<long long> pulses(blocks.size(), -1);
	std::vector<char> taken(blocks.size(), 0);
	auto w16 = [&](size_t at) -> int {	// fgetw; bodies are proven, this is a guard
		return at + 1 < v.size() ? (int)rd16le(v, at) : 0;
	};
	int pc = 0, steps = 0;
	int loopPc = -1, loopCount = 0, callPc = -1, callIdx = 0, callCnt = 0;
	long long total = 0;
	while (pc >= 0 && pc < cnt && steps++ < 0x100000) {	// TZX_MAX_STEPS
		const size_t p = blocks[pc].body;
		switch (blocks[pc].id) {
			case 0x23: {
				const int off = (short)w16(p);
				if (off == 0 || taken[pc]) {
					pc = cnt;
				} else {
					taken[pc] = 1;
					pc += off;
				}
				break;
			}
			case 0x24:
				loopCount = w16(p);
				loopPc = pc + 1;
				pc++;
				break;
			case 0x25:
				if (loopPc >= 0 && loopCount > 1) {
					loopCount--;
					pc = loopPc;
				} else {
					loopPc = -1;
					pc++;
				}
				break;
			case 0x26:
				callCnt = w16(p);
				if (callCnt > 0) {
					callPc = pc;
					callIdx = 0;
					pc += (short)w16(p + 2);
				} else {
					pc++;
				}
				break;
			case 0x27:
				if (callPc < 0) { pc++; break; }
				callIdx++;
				if (callIdx < callCnt) {
					pc = callPc + (short)w16(blocks[callPc].body + 2 + callIdx * 2);
				} else {
					pc = callPc + 1;
					callPc = -1;
				}
				break;
			default:
				if (pulses[pc] < 0) pulses[pc] = tzxBlockPulses(v, p, blocks[pc].id);
				total += pulses[pc];
				if (total > kTzxMaxPulses) tzxTooLong();
				pc++;
				break;
		}
	}
}

// .ima - a fixed 160 tracks of 18x512: anything shorter leaves the tail of
// the last sector buffer formatted in. The format also fails silently when
// the drive cannot hold 9216 bytes of track (all of the core's drives are
// DD, trklen 6250), which load_ima reports as success on the stale disk -
// checked at load time where the drive exists.
static void checkImaFile(const std::string& path) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) throw std::runtime_error("cannot open '" + path + "'");
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fclose(f);
	if (n < 160L * 18 * 512)
		throw std::runtime_error("truncated .ima: " + std::to_string(n) +
					 " of 1474560 bytes");
}

// .bin - the BK-style loader old xpeccy shipped as loadBIN() and the fork
// dropped. Kept verbatim: 16-bit little-endian load address and length, then
// the payload read as words and written with memWr - which writes one byte,
// the word's low half, at the even address and steps two. A Spectrum caller
// therefore sees every other file byte land; that is exactly what the
// upstream loader did, quirk included, and this tool exists for compatibility
// with it, not to redefine the format. (The original also set the BK's
// regNOD video register, which the fork removed with the BK hardware itself.)
static int loadBinFile(Computer* comp, const std::string& path) {
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return ERR_CANT_OPEN;
	int err = ERR_OK;
	int adr = fgetw(file) & ~1;
	int len = fgetw(file);
	if (len & 1) len++;
	if (adr + len > 0x8000) {
		err = ERR_RAW_LONG;
	} else {
		while (len > 0) {
			const int val = fgetw(file);
			memWr(comp->mem, adr, val);
			adr += 2;
			len -= 2;
		}
	}
	fclose(file);
	return err;
}

// fm_takes() headless: can THIS machine consume the format. A hardware id
// alone is not enough - upstream checks the controller too, because a
// definition that patched its disk away leaves the file to "load" onto a
// machine with no drive to read it.
static bool fileRunsOn(const std::string& ext, const std::string& path, Computer* c) {
	if (!c || !c->hw) return false;
	if (ext == "spg") return c->hw->id == HW_TSLAB;
	if (ext == "sna" || ext == "z80") {
		const int snap = (ext == "sna") ? snaGetHardware(path.c_str())
						: z80GetHardware(path.c_str());
		return snap >= 0 && snapHwRuns(snap, c->hw->id);
	}
	if (ext == "dsk")	// autostart_can(hw, dif, AS_DISK3): only a +3 row exists
		return c->hw->id == HW_PLUS3 && c->dif && c->dif->type == DIF_P3DOS;
	if (ext == "trd" || ext == "scl" || ext == "fdi" || ext == "td0" || ext == "udi") {
		if (!c->dif || c->dif->type != DIF_BDI) return false;
		// as_mtab's AS_DISK rows: hardware that can autostart a TR-DOS disk
		switch (c->hw->id) {
			case HW_ZX48: case HW_ZX128: case HW_PENT: case HW_P1024:
			case HW_PROFI: case HW_PROFI3: case HW_PHOENIX: case HW_SCORP:
			case HW_SCORPTP: case HW_TSLAB: case HW_ATM2: case HW_PENTEVO:
				return true;
		}
		return false;
	}
	return true;	// tapes, .ima, .bin: FMN_ANY
}

// The hardware id a definition's `hw` name resolves to, in the same table
// the shipped-machine list is ordered by
static int hwIdForName(const std::string& hw) {
	for (int i = 0; tabHwPtr[i].id != HW_NULL; i++)
		if (tabHwPtr[i].core && hw == tabHwPtr[i].core->name)
			return tabHwPtr[i].id;
	return HW_NULL;
}

// The same question answered from the definition, without switching to it:
// a candidate judged by applying it would cold-fill RAM on every miss, so a
// failed load used to wipe the session's memory before it could say no
static bool defRunsOn(const std::string& ext, const std::string& path, const MachineDef& d) {
	const int hwId = hwIdForName(d.hw);
	if (hwId == HW_NULL) return false;
	if (ext == "spg") return hwId == HW_TSLAB;
	if (ext == "sna" || ext == "z80") {
		const int snap = (ext == "sna") ? snaGetHardware(path.c_str())
						: z80GetHardware(path.c_str());
		return snap >= 0 && snapHwRuns(snap, hwId);
	}
	if (ext == "dsk") return hwId == HW_PLUS3 && d.disk == DIF_P3DOS;
	if (ext == "trd" || ext == "scl" || ext == "fdi" || ext == "td0" || ext == "udi") {
		if (d.disk != DIF_BDI) return false;
		switch (hwId) {
			case HW_ZX48: case HW_ZX128: case HW_PENT: case HW_P1024:
			case HW_PROFI: case HW_PROFI3: case HW_PHOENIX: case HW_SCORP:
			case HW_SCORPTP: case HW_TSLAB: case HW_ATM2: case HW_PENTEVO:
				return true;
		}
		return false;
	}
	return true;
}

// The machine a file is opened on - xcore/filemachine.cpp fm_pick(), headless.
// The current machine keeps the file when it can run it; when it cannot, the
// machine the file was made for comes up first (definition stem, hardware name
// as the fallback) and is reset the way xm_set() resets it - but only when the
// machine it lands on actually consumes the format: a user-patched definition
// that dropped its disk controller is a name, not a drive.
static void machineForFile(const std::string& ext, const std::string& path) {
	if (fileRunsOn(ext, path, mach().comp())) return;
	struct Target { const char* def; const char* hw; };
	Target target{nullptr, nullptr};
	if (ext == "spg") {					// FMN_TSCONF
		target = {"evo-tsconf", "TSConf"};
	} else if (ext == "sna" || ext == "z80") {		// FMN_SNAPSHOT
		int snap = (ext == "sna") ? snaGetHardware(path.c_str())
					: z80GetHardware(path.c_str());
		switch (snap) {
			case SNAP_HW_48K:	target = {"zx48", "ZX48"}; break;
			case SNAP_HW_128K:	target = {"zx128", "ZX128"}; break;
			case SNAP_HW_PLUS2:	target = {"zxplus2", "ZX128"}; break;
			case SNAP_HW_PLUS2A:	target = {"zxplus2a", "Plus2A"}; break;
			case SNAP_HW_PLUS3:	target = {"zxplus3", "Plus3"}; break;
			case SNAP_HW_PENTAGON:	target = {"pent", "Pentagon"}; break;
			case SNAP_HW_SCORPION:	target = {"scorp", "Scorpion"}; break;
			default: return;	// the file says nothing: it takes its chances here
		}
	} else if (ext == "trd" || ext == "scl" || ext == "fdi" ||
		   ext == "td0" || ext == "udi") {			// FMN_TRDOS
		target = {"pent", "Pentagon"};
	} else if (ext == "dsk") {					// FMN_PLUS3DOS
		target = {"zxplus3", "Plus3"};
	} else {
		return;				// tapes, .ima, .bin: FMN_ANY
	}
	const Env& env = mach().env();
	// candidates are judged on their definitions, the named stem first and
	// then the whole list in machine order the way fm_pick scans it - so a
	// clone definition of the right hardware qualifies even when the stock
	// one was patched into uselessness, and nothing is applied on a miss
	std::vector<const MachineDef*> cand;
	if (target.def)
		if (const MachineDef* d = env.findMachine(target.def)) cand.push_back(d);
	for (const auto& d : env.machines)
		if (!target.def || d.id != target.def) cand.push_back(&d);
	std::string err;
	bool ok = false;
	const std::string wasDef = mach().machineDefId();
	const std::string wasHw = mach().model();
	for (const MachineDef* d : cand) {
		if (!defRunsOn(ext, path, *d)) continue;
		if (mach().setModel(d->id, err) &&
		    fileRunsOn(ext, path, mach().comp())) { ok = true; break; }
	}
	// no definitions at all: the hardware name is still worth one switch
	if (!ok && cand.empty() && target.hw &&
	    mach().setModel(target.hw, err))
		ok = fileRunsOn(ext, path, mach().comp());
	if (!ok) {
		// a candidate that claimed compatibility but failed its live check
		// still switched the machine - put the session's one back. But only
		// if it did: re-applying the same model would cold-fill RAM and wipe
		// the very state the refusal was meant to preserve
		const bool moved = mach().machineDefId() != wasDef ||
				   mach().model() != wasHw;
		if (moved) {
			std::string ignored;
			if (!wasDef.empty()) mach().setModel(wasDef, ignored);
			else if (!wasHw.empty()) mach().setModel(wasHw, ignored);
		}
		throw std::runtime_error("a ." + ext +
					 " file needs a machine that can consume it "
					 "(the right hardware and controller): " + err);
	}
	mach().reset();				// what xm_set() does on the same switch
}

// The core's run-ahead snapshot, loaded back after a failed load_file. Its
// ranges were drawn for a rollback inside one machine, and two things differ
// when the machine was switched and switched back in between:
//  - the RAM mask is what the definition set, and some boards widen it only
//    when their memory is mapped (ZX48 reaches banks 2 and 5 behind a 128K
//    mask) - a layout the snapshot refuses until the board maps once;
//  - a few fields inside the ranges own heap memory that a switch or a
//    snapshot loader reallocates: the heat counters (sized to the RAM mask),
//    the text font, the snapshot path. Their saved values are freed pointers
//    by now, so the live ones stay, and the counters are sized to the mask
//    the snapshot puts back.
static bool restoreMachineState(xState* st, Computer* c) {
	if (c->hw && c->hw->mapMem) c->hw->mapMem(c);
	const xHeatBank heatRam = c->heatRam, heatRom = c->heatRom;
	const auto font = c->vid->font;
	char* const snapath = c->mem->snapath;
	const bool ok = xstate_load(st, c) != 0;
	c->heatRam = heatRam;
	c->heatRom = heatRom;
	c->vid->font = font;
	c->mem->snapath = snapath;
	if (ok) comp_heat_sync(c);
	return ok;
}

void registerMediaTools() {
	tool("load_file",
	     "Load a file into the machine by extension: .sna/.z80 snapshots, .spg (TSConf/Profi), "
	     ".tap/.tzx/.wav tapes, .trd/.scl/.udi/.fdi/.td0/.dsk/.ima disks (drive A by default), "
	     ".bin (BK-style: little-endian load address and length header, then the payload into "
	     "RAM). Snapshots start running immediately. When the file's format names a "
	     "machine the current one cannot run (an .spg wants TSConf, a .trd wants a Beta "
	     "Disk controller), the machine switches to it first, the way upstream's "
	     "filemachine does.",
	     json{{"properties", {
		     {"path", {{"type", "string"}}},
		     {"drive", {{"type", "integer"}, {"description", "disk drive 0..3, default 0"}}}
	     }}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string path = argStr(a, "path");
		     if (path.empty()) throw std::runtime_error("path required");
		     std::string ext;
		     size_t dot = path.find_last_of('.');
		     if (dot != std::string::npos) ext = path.substr(dot + 1);
		     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
		     int drv = argNumOr(a, "drive", 0);
		     if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
		     long fmtNeed = 0;		// smallest trklen the image fits
		     std::vector<DskShortTrack> dskShort;
		     if (ext == "spg") checkSpgFile(path);	// before it can switch the machine
		     else if (ext == "td0") fmtNeed = checkTd0File(path);
		     else if (ext == "dsk") fmtNeed = checkDskFile(path, &dskShort);
		     else if (ext == "fdi") fmtNeed = checkFdiFile(path);
		     else if (ext == "udi") checkUdiFile(path);
		     else if (ext == "trd") checkTrdFile(path);
		     else if (ext == "scl") checkSclFile(path);
		     else if (ext == "tap") checkTapFile(path);
		     else if (ext == "tzx") checkTzxFile(path);
		     else if (ext == "sna") checkSnaFile(path);
		     else if (ext == "z80") checkZ80File(path);
		     else if (ext == "ima") checkImaFile(path);
		     // diskFormTrack quietly keeps the old track when the declared
		     // sectors do not fit its length - refuse the image instead of
		     // reporting success over it. The drive is known before any
		     // machine is: every DD image goes into a DD drive (see below)
		     if (fmtNeed > TRKLEN_DD)
			     throw std::runtime_error("bad ." + ext + ": a track needs " +
						      std::to_string(fmtNeed) +
						      " bytes of room, the drive packs " +
						      std::to_string(TRKLEN_DD) +
						      " (the core formats it silently "
						      "over the old track)");
		     const bool diskImage = ext == "trd" || ext == "scl" || ext == "udi" ||
					    ext == "fdi" || ext == "td0" || ext == "dsk" ||
					    ext == "ima";
		     // a failed load is not allowed to cost the session: remember
		     // which machine and which disks were mounted, so a refusal
		     // past this point can put both back - a machine switch can
		     // eject drives its drive count does not fit
		     struct MountState { int insert, changed; long trklen; std::string path; };
		     MountState mounts[4] = {};
		     std::vector<unsigned char> keptData;
		     {
			     Computer* cur = mach().comp();
			     if (cur->dif && cur->dif->fdc) {
				     for (int i = 0; i < 4; i++) {
					     Floppy* f = cur->dif->flp[i];
					     mounts[i] = {f->insert, f->changed, f->trklen,
							  f->path ? f->path : ""};
				     }
				     // the target drive is the only one a loader can write,
				     // and one that fails mid-format has already taken over
				     // its path and half its tracks - keep the disk's bytes
				     if (diskImage && mounts[drv].insert) {
					     const Floppy* f = cur->dif->flp[drv];
					     const unsigned char* p =
						     reinterpret_cast<const unsigned char*>(f->data);
					     keptData.assign(p, p + sizeof(f->data));
				     }
			     }
		     }
		     const std::string wasDef = mach().machineDefId();
		     const std::string wasHw = mach().model();
		     // the rest of the machine: a switch cold-fills RAM, and a
		     // snapshot loader that gives up has already reset the CPU and
		     // written pages. The core's run-ahead snapshot holds exactly
		     // RAM, paging, CPU and chips - not media, which the mounts
		     // above cover - and refuses to load onto a different machine
		     std::unique_ptr<xState, void (*)(xState*)> before(xstate_create(), xstate_destroy);
		     const bool saved = before && xstate_save(before.get(), mach().comp());
		     Computer* c;
		     int res;
		     try {
		     machineForFile(ext, path);
		     c = mach().comp();	// after the switch: the pointer changed
		     // zx_init gives every drive DD geometry and only .ima asks for
		     // HD: a drive an .ima left HD would format the next image at
		     // 17700 and the geometry noted after the load would keep it
		     if (diskImage && ext != "ima" && c->dif && c->dif->fdc)
			     flp_set_hd(c->dif->flp[drv & 3], 0);
		     if (ext == "sna") res = loadSNA(c, path.c_str(), drv);
		     else if (ext == "z80") res = loadZ80(c, path.c_str(), drv);
		     else if (ext == "spg") res = loadSPG(c, path.c_str(), drv);
		     else if (ext == "tap") res = loadTAP(c, path.c_str(), drv);
		     else if (ext == "tzx") res = loadTZX(c, path.c_str(), drv);
		     else if (ext == "wav") res = loadWAV(c, path.c_str(), drv);
		     else if (ext == "trd") {
			     res = loadTRD(c, path.c_str(), drv);
			     if (res == ERR_OK)
				     fixupTrdTail(c->dif->flp[drv & 3], path);
		     }
		     else if (ext == "scl") res = loadSCL(c, path.c_str(), drv);
		     else if (ext == "udi") res = loadUDI(c, path.c_str(), drv);
		     else if (ext == "fdi" || ext == "td0" || ext == "dsk") {
			     // the fit was checked against TRKLEN_DD before the switch
			     Floppy* flp = (c->dif && c->dif->fdc)
				     ? c->dif->flp[drv & 3] : nullptr;
			     if (!flp)
				     throw std::runtime_error("." + ext + " needs a machine "
							      "with a disk interface");
			     res = (ext == "fdi") ? loadFDI(c, path.c_str(), drv)
				: (ext == "td0") ? loadTD0(c, path.c_str(), drv)
				               : loadDSK(c, path.c_str(), drv);
			     if (res == ERR_OK && ext == "dsk")
				     fixupDskShort(flp, dskShort);
		     }
		     else if (ext == "ima") {
			     // 18x512 sectors per track need ~10.8K of track room;
			     // the core's drives are created DD (trklen 6250), where
			     // flp_format_trk fails on every track and load_ima still
			     // reports success - the "inserted" disk is whatever was
			     // there before. Give the drive the HD geometry the
			     // image was written for, then verify it formatted.
			     Floppy* flp = (c->dif && c->dif->fdc)
				     ? c->dif->flp[drv & 3] : nullptr;
			     if (!flp)
				     throw std::runtime_error(".ima needs a machine "
							      "with a disk interface");
			     // flp_set_hd, not fdc_set_hd: the latter re-times
			     // the whole FDC (bytedelay) and all four drives,
			     // a mounted DD disk would silently change shape
			     flp_set_hd(flp, 1);
			     res = load_ima(c, path.c_str(), drv);
			     if (res == ERR_OK) {
				     unsigned char sec[512];
				     if (!diskGetSectorData(flp, 0, 1, sec, sizeof(sec)))
					     throw std::runtime_error("bad .ima: the "
							      "drive could not format its tracks");
			     }
		     }
		     else if (ext == "bin") res = loadBinFile(c, path);
		     else throw std::runtime_error("unsupported extension '" + ext + "'");
		     if (res != ERR_OK) throw std::runtime_error("loader failed with code " + std::to_string(res));
		     } catch (...) {
			     // the machine went first so a valid file lands on a
			     // machine that can run it; the refusal above is ours,
			     // so the switch's side effects go back with it. The
			     // restore itself may fail - the mounts still deserve
			     // their seats back, and the original error the throw
			     try {
				     if (mach().machineDefId() != wasDef ||
					 mach().model() != wasHw) {
					     std::string ignored;
					     if (!wasDef.empty()) mach().setModel(wasDef, ignored);
					     else if (!wasHw.empty()) mach().setModel(wasHw, ignored);
				     }
			     } catch (...) {}
			     Computer* cur = mach().comp();
			     if (cur->dif && cur->dif->fdc)
				     for (int i = 0; i < 4; i++) {
					     Floppy* f = cur->dif->flp[i];
					     if (mounts[i].insert) {
						     if (!f->insert) {
							     // flp_eject cleared the flags, not the
							     // track data - put the flags back the
							     // way flp_insert does, without the
							     // clearDisk a null-path insert does
							     f->insert = 1;
							     f->door = 0;
							     f->dwait = 5e8;
							     flp_set_path(f, mounts[i].path.empty()
								     ? nullptr : mounts[i].path.c_str());
							     f->xirq(IRQ_FDD_RDY, f->xptr);
						     }
						     if (i == drv && !keptData.empty()) {
							     // a still-inserted target holds the failed
							     // file's path over half-written tracks -
							     // hand back the bytes and the name
							     memcpy(f->data, keptData.data(),
								    sizeof(f->data));
							     flp_set_path(f, mounts[i].path.empty()
								     ? nullptr : mounts[i].path.c_str());
						     }
						     f->changed = mounts[i].changed;
						     // a surviving drive may still have lost
						     // its geometry - zx_init marks every
						     // drive DD, and a failed .ima left HD
						     f->trklen = mounts[i].trklen;
						     mach().noteDriveGeom(i, f->trklen);
					 } else if (f->insert) {
					     // an empty drive the failed loader mounted its
					     // half-written work on goes back to empty
					     flp_eject(f);
					     f->trklen = mounts[i].trklen;
					 } else {
					     f->trklen = mounts[i].trklen;   // a failed .ima left HD
					 }
				     }
			     // RAM, paging, CPU - only onto the machine they were taken
			     // from: a restore that did not get back there leaves a
			     // machine the bytes would not fit
			     if (saved && mach().machineDefId() == wasDef && mach().model() == wasHw)
				     restoreMachineState(before.get(), mach().comp());
			     throw;
		     }
		     // the load succeeded - a bookkeeping error past this point is
		     // not a reason to undo it: the disk the loader just wrote must
		     // keep the geometry it was formatted with
		     c = mach().comp();
		     if (diskImage) {
			     mach().noteDriveGeom(drv & 3, c->dif->flp[drv & 3]->trklen);
			     mach().faults().forget(drv & 3);
		     }
		     // snapshot loaders compReset internally -> zx_init -> DD
		     // geometry on every drive; put back what the mounts need
		     mach().reapplyDriveGeom();
		     // the frames still in the history were drawn by whatever ran before
		     xsp::video::historyClear();
		     return json{{"path", path}, {"format", ext}, {"ok", true},
			     {"machine", mach().machineDefId()}, {"model", mach().model()},
			     {"pc", cpu_get_pc(c->cpu)}, {"pc_hex", hex16(cpu_get_pc(c->cpu))}};
	     });

	tool("tape",
	     "Tape deck: state (what is loaded, which block, is it playing), play, stop, rewind, next "
	     "block, eject, or blocks to list what is on the tape. A .tap/.tzx must be loaded first "
	     "with load_file - loading does not press play.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "state | play | stop | rewind | next | eject | blocks"}}},
		     {"block", {{"type", "integer"}, {"description", "block to rewind to, default 0"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = mach().comp();
		     Tape* t = c->tape;
		     std::string action = argStr(a, "action", "state");
		     // The core stores whatever block number it is handed and indexes
		     // blkData with it on the next play, so the range check has to
		     // happen here - upstream has none, and -1 is a segfault one call
		     // later rather than an error on the call that caused it.
		     if (action == "play") {
			     if (t->block < 0 || t->block >= t->blkCount)
				     throw std::runtime_error("tape is not at a playable block (" +
							      std::to_string(t->block) + " of " +
							      std::to_string(t->blkCount) + ")");
			     tapPlay(t);
		     }
		     else if (action == "stop") tapStop(t);
		     else if (action == "rewind") {
			     if (t->blkCount < 1) throw std::runtime_error("no tape loaded - see load_file");
			     tapRewind(t, argNumRange(a, "block", 0, 0, t->blkCount - 1));
		     }
		     else if (action == "next") tapNextBlock(t);
		     else if (action == "eject") tapEject(t);
		     else if (action != "state" && action != "blocks")
			     throw std::runtime_error("unknown tape action '" + action + "'");

		     json res{
			     {"path", t->path ? t->path : ""},
			     {"playing", t->on ? true : false},
			     {"recording", t->rec ? true : false},
			     {"block", t->block},
			     {"blocks", t->blkCount}
		     };
		     if (action == "blocks") {
			     std::vector<TapeBlockInfo> info((size_t)std::max(0, t->blkCount));
			     int n = info.empty() ? 0 : tapGetBlocksInfo(t, info.data());
			     json list = json::array();
			     for (int i = 0; i < n; i++)
				     list.push_back({{"index", i}, {"name", std::string(info[i].text)},
						     {"type", info[i].type}, {"size", info[i].size},
						     {"time_ms", info[i].time / 1000000}});
			     res["block_list"] = list;
		     }
		     return res;
	     });

	tool("disk",
	     "Floppy drives: state, insert a .trd/.scl image, eject, or save the current image back to "
	     "a file. Drive 0 is A. setup changes the drive itself: angle_deg turns the disk (where "
	     "the next command starts), rpm_percent makes it faster or slower, spinup_ms delays a "
	     "command while the motor comes up to speed, write_protect.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "state | insert | eject | save | setup"}}},
		     {"drive", {{"type", "integer"}, {"description", "0..3, default 0"}}},
		     {"path", {{"type", "string"}}},
		     {"angle_deg", {{"type", "number"}}},
		     {"rpm_percent", {{"type", "number"}, {"description", "-10..10"}}},
		     {"spinup_ms", {{"type", "integer"}, {"description", "0..1000"}}},
		     {"write_protect", {{"type", "boolean"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = mach().comp();
		     int drv = argNumOr(a, "drive", 0);
		     if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
		     Floppy* flp = c->dif->fdc->flop[drv];
		     std::string action = argStr(a, "action", "state");
		     std::string path = argStr(a, "path");

		     if (action == "insert") {
			     if (path.empty()) throw std::runtime_error("path required");
			     std::string ext;
			     size_t dot = path.find_last_of('.');
			     if (dot != std::string::npos) ext = path.substr(dot + 1);
			     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
			     // the same unchecked core parsers load_file guards
			     long fmtNeed = 0;	// smallest trklen the image fits
			     std::vector<DskShortTrack> dskShort;
			     if (ext == "td0") fmtNeed = checkTd0File(path);
			     else if (ext == "dsk") fmtNeed = checkDskFile(path, &dskShort);
			     else if (ext == "fdi") fmtNeed = checkFdiFile(path);
			     else if (ext == "udi") checkUdiFile(path);
			     else if (ext == "trd") checkTrdFile(path);
			     else if (ext == "scl") checkSclFile(path);
			     else if (ext == "ima") checkImaFile(path);
			     // the formats above write their tracks through
			     // diskFormTrack - a track that does not fit the
			     // drive's trklen is skipped silently while the load
			     // still reports success. Everything but .ima goes into
			     // a DD drive, set below
			     if (fmtNeed > TRKLEN_DD)
				     throw std::runtime_error("bad ." + ext +
					": a track needs " + std::to_string(fmtNeed) +
					" bytes of room, the drive packs " +
					std::to_string(TRKLEN_DD));
			     // the drive the loader writes is dif->flp[drv]; a file
			     // that fails mid-format has already taken its path and
			     // half its tracks - keep the disk it is replacing so a
			     // failed insert can give it back
			     Floppy* tgt = c->dif->flp[drv & 3];
			     const int wasIn = tgt->insert, wasChg = tgt->changed;
			     const long wasTrklen = tgt->trklen;
			     const std::string wasPath = tgt->path ? tgt->path : "";
			     std::vector<unsigned char> keptData;
			     if (wasIn) {
				     const unsigned char* b =
					     reinterpret_cast<const unsigned char*>(tgt->data);
				     keptData.assign(b, b + sizeof(tgt->data));
			     }
			     int res;
			     try {
			     // the DD geometry zx_init gives every drive: an .ima
			     // before this one would otherwise have the image
			     // formatted at its HD length, see load_file
			     if (ext != "ima") flp_set_hd(tgt, 0);
			     if (ext == "scl") res = loadSCL(c, path.c_str(), drv);
			     else if (ext == "udi") res = loadUDI(c, path.c_str(), drv);
			     else if (ext == "fdi") res = loadFDI(c, path.c_str(), drv);
			     else if (ext == "td0") res = loadTD0(c, path.c_str(), drv);
			     else if (ext == "dsk") {
				     res = loadDSK(c, path.c_str(), drv);
				     if (res == ERR_OK) fixupDskShort(tgt, dskShort);
			     }
			     else if (ext == "ima") {
				     // only this drive, see load_file
				     flp_set_hd(tgt, 1);
				     res = load_ima(c, path.c_str(), drv);
				     if (res == ERR_OK) {
					     unsigned char sec[512];
					     if (!diskGetSectorData(tgt, 0, 1,
								    sec, sizeof(sec)))
						     throw std::runtime_error("bad .ima: the drive "
									      "could not format its tracks");
				     }
			     }
			     else {
				     res = loadTRD(c, path.c_str(), drv);	// .trd and anything else
				     if (res == ERR_OK)
					     fixupTrdTail(tgt, path);
			     }
			     if (res != ERR_OK)
				     throw std::runtime_error("loader failed with code " + std::to_string(res));
			     } catch (...) {
				     if (wasIn) {
					     // hand the replaced disk back - flp_eject
					     // clears the flags, not the tracks, and a
					     // null-path flp_insert would clear them, so
					     // re-seat by hand
					     if (!tgt->insert) {
						     tgt->insert = 1;
						     tgt->door = 0;
						     tgt->dwait = 5e8;
						     tgt->xirq(IRQ_FDD_RDY, tgt->xptr);
					     }
					     if (!keptData.empty())
						     memcpy(tgt->data, keptData.data(),
							    sizeof(tgt->data));
					     tgt->changed = wasChg;
					     tgt->trklen = wasTrklen;
					     flp_set_path(tgt, wasPath.empty()
						     ? nullptr : wasPath.c_str());
				     } else {
					     if (tgt->insert) flp_eject(tgt);	// a half-written mount
					     tgt->trklen = wasTrklen;	// a failed .ima left HD
				     }
				     mach().reapplyDriveGeom();
				     throw;
			     }
			     // the drive's geometry is the one its image was just
			     // written with - remember it before reapplying the
			     // others', in case a loader's zx_init shrunk them
			     mach().noteDriveGeom(drv, c->dif->flp[drv & 3]->trklen);
			     mach().reapplyDriveGeom();
			     mach().faults().forget(drv);	// they were on the disk that went out
		     } else if (action == "eject") {
			     mach().faults().forget(drv);
			     flp->insert = 0;
			     // the HD length an .ima set goes with the disk: the next
			     // image comes into the DD drive zx_init would have made
			     flp_set_hd(flp, 0);
			     mach().noteDriveGeom(drv, 0);
		     } else if (action == "save") {
			     if (path.empty()) throw std::runtime_error("path required");
			     int res = saveTRD(c, path.c_str(), drv);
			     if (res != ERR_OK)
				     throw std::runtime_error("save failed with code " + std::to_string(res));
		     } else if (action == "setup") {
			     auto& tw = mach().fdc().tweak(drv);
			     double v = 0;
			     if (argDouble(a, "rpm_percent", v)) {
				     if (v < -10 || v > 10) throw std::runtime_error("rpm_percent must be -10..10");
				     tw.rpmPercent = v;
			     }
			     if (a.contains("spinup_ms")) tw.spinupMs = argNumRange(a, "spinup_ms", 0, 0, 1000);
			     if (a.contains("write_protect")) flp->protect = argBool(a, "write_protect", false) ? 1 : 0;
			     if (argDouble(a, "angle_deg", v)) {
				     if (!flp->insert || flp->trklen <= 0)
					     throw std::runtime_error("no disk in drive " + std::to_string(drv) + " to turn");
				     double d = std::fmod(v, 360.0);
				     if (d < 0) d += 360.0;
				     flp->pos = (int)std::lround(d / 360.0 * flp->trklen) % flp->trklen;
				     flp->index = flp->pos < 4 ? 1 : 0;
			     }
		     } else if (action != "state") {
			     throw std::runtime_error("unknown disk action '" + action + "'");
		     }
		     // an inserted image the machine has no controller for reads as a working setup
		     // right up to the point the ROM waits for a drive that will never answer
		     json st = {{"drive", drv}, {"inserted", flp->insert ? true : false},
			     {"path", flp->path ? flp->path : ""},
			     {"changed", flp->changed ? true : false},
			     {"track_bytes", flp->trklen},
			     {"interface", diskInterfaceName(c->dif->type)}};
		     if (flp->insert && flp->trklen > 0)
			     st["angle_deg"] = std::round(flp->pos * 3600.0 / flp->trklen) / 10.0;
		     if (flp->protect) st["write_protect"] = true;
		     const auto& tw = mach().fdc().tweak(drv);
		     if (tw.rpmPercent != 0.0) st["rpm_percent"] = tw.rpmPercent;
		     if (tw.spinupMs) st["spinup_ms"] = tw.spinupMs;
		     const size_t nf = mach().faults().list(drv).size();
		     if (nf) {
			     st["faults"] = nf;
			     // a saved image carries them: they are bytes on the tracks
			     if (action == "save")
				     st["warning"] = std::to_string(nf) + " fault(s) from disk_track are on this disk "
						     "and were saved with it";
		     }
		     return st;
	     });

	tool("disk_catalog",
	     "The TR-DOS catalogue of the disk in a drive: file names, extensions, start address, "
	     "length and where they sit on the disk.",
	     json{{"properties", {{"drive", {{"type", "integer"}}}}}},
	     [](const json& a) {
		     int drv = argNumOr(a, "drive", 0);
		     if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
		     Floppy* flp = mach().comp()->dif->fdc->flop[drv];
		     if (!flp->insert) throw std::runtime_error("no disk in drive " + std::to_string(drv));
		     std::vector<TRFile> cat(128);
		     int n = diskGetTRCatalog(flp, cat.data());
		     json list = json::array();
		     for (int i = 0; i < n; i++) {
			     const TRFile& f = cat[i];
			     std::string name((const char*)f.name, 8);
			     while (!name.empty() && name.back() == ' ') name.pop_back();
			     list.push_back({
				     {"name", name},
				     {"ext", std::string(1, (char)f.ext)},
				     {"start", (f.hst << 8) | f.lst},
				     {"start_hex", hex16((f.hst << 8) | f.lst)},
				     {"length", (f.hlen << 8) | f.llen},
				     {"sectors", f.slen},
				     {"track", f.trk},
				     {"sector", f.sec}
			     });
		     }
		     return json{{"drive", drv}, {"files", (int)n}, {"catalog", list}};
	     });

	tool("sdcard",
	     "SD card (TSConf/ZX-Evo, ATM, Profi): state, mount an image file, eject, or write-protect. "
	     "The image is a raw card file, sector-addressed; mount it before boot so the machine's "
	     "SD controller finds it.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "state | mount | eject | lock | unlock"}}},
		     {"path", {{"type", "string"}, {"description", "image file for mount"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = mach().comp();
		     SDCard* s = c->sdc;
		     std::string action = argStr(a, "action", "state");
		     std::string path = argStr(a, "path");

		     if (action == "mount") {
			     if (path.empty()) throw std::runtime_error("path required");
			     if (!xsp::platform::isFile(path))
				     throw std::runtime_error("no such image file: " + path);
			     // the core sizes the card with signed ints: ftell narrowed
			     // to int, then a doubling loop up to the file size.
			     // Exactly 1 GiB stops at 2^30; one byte more needs 2^31,
			     // the shift overflows and the loop spins forever, and a
			     // 2 GiB file reports capacity 0.
			     FILE* probe = fopen(path.c_str(), "rb");
			     if (!probe)
				     throw std::runtime_error("cannot open image: " + path);
			     fseek(probe, 0, SEEK_END);
			     const long sz = ftell(probe);
			     fclose(probe);
			     if (sz < 0 || sz > (1L << 30))
				     throw std::runtime_error("image too large: " +
							      std::to_string(sz) + " bytes (the core's "
							      "card sizing is 32-bit; images must be "
							      "at most 1 GiB)");
			     sdcSetImage(s, path.c_str());
			     // a swapped card drops whatever transfer was in flight
			     // and powers up uninitialized, waiting for ACMD41 - and
			     // a new card is not born write-protected
			     sdcReset(s);
			     s->idle = 1;
			     s->lock = 0;
			     if (!s->file)
				     throw std::runtime_error("could not open image (locked by OS?): " + path);
		     } else if (action == "eject") {
			     sdcSetImage(s, "");
			     sdcReset(s);
			     s->idle = 1;
		     } else if (action == "lock" || action == "unlock") {
			     sdcSetLock(s, action == "lock");
		     } else if (action != "state") {
			     throw std::runtime_error("unknown sdcard action '" + action + "'");
		     }
		     // sdcSetImage("") keeps the last card's capacity and lock: report
		     // them only while a card is actually in, image file or folder
		     const bool mounted = (s->file != nullptr) || (s->vfat != nullptr);
		     return json{{"mounted", mounted},
			     {"image", mounted && s->image ? s->image : ""},
			     {"capacity_mb", mounted ? s->capacity : 0},
			     {"locked", mounted && s->lock ? true : false}};
	     });

	tool("save_snapshot",
	     "Save the current machine state to a .sna file. The standard way to skip a long "
	     "precalculation: run once to the main loop, save here, and load that snapshot for "
	     "every later measurement instead of paying for the precalculation each time.",
	     json{{"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string path = argStr(a, "path");
		     Computer* c = mach().comp();
		     int res = saveSNA(c, path.c_str(), 0);
		     if (res != ERR_OK) throw std::runtime_error("save failed with code " + std::to_string(res));
		     json j = {{"path", path}, {"ok", true}};
		     const FDC* f = c->dif ? c->dif->fdc : nullptr;
		     if (f && f->plan && !f->idle)
			     j["warning"] = "the disk controller is in the middle of a command, and a .sna has no "
					    "room for it or for the angle of the disk: a load in progress will not "
					    "resume from this file. checkpoint keeps both (in this server process)";
		     return j;
	     });
}

} // namespace tools
} // namespace xsp
