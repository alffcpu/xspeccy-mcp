#include "xsp_record.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

#ifdef _WIN32
	#define XSP_POPEN _popen
	#define XSP_PCLOSE _pclose
	// Raw RGBA goes down this pipe, so it must be binary: a text-mode pipe expands
	// every 0x0A byte to 0x0D 0x0A (measured on MinGW-w64: 300 bytes in, 302 out),
	// which shifts every frame after the first such pixel and hands ffmpeg garbage.
	// It costs nothing today only because the ZX palette we install is 0x00/0xaa/
	// 0xff and alpha 0xff, so a frame never contains a 10 - a palette that did
	// would corrupt recordings silently, and be very hard to read back to here.
	#define XSP_PIPE_WRITE "wb"
#else
	#define XSP_POPEN popen
	#define XSP_PCLOSE pclose
	#define XSP_PIPE_WRITE "w"
#endif

namespace xsp {
namespace record {

// ---------------------------------------------------------------- GIF

// GIF LZW: variable-width codes packed LSB-first into 255-byte sub-blocks.
namespace {

class LzwOut {
public:
	explicit LzwOut(FILE* f) : m_file(f) {}

	void putCode(int code, int width) {
		m_acc |= (uint32_t)code << m_bits;
		m_bits += width;
		while (m_bits >= 8) {
			m_block[m_blockLen++] = (unsigned char)(m_acc & 0xff);
			m_acc >>= 8;
			m_bits -= 8;
			if (m_blockLen == 255) flushBlock();
		}
	}

	void finish() {
		while (m_bits > 0) {
			m_block[m_blockLen++] = (unsigned char)(m_acc & 0xff);
			m_acc >>= 8;
			m_bits -= 8;
			if (m_blockLen == 255) flushBlock();
		}
		if (m_blockLen) flushBlock();
		fputc(0x00, m_file);		// block terminator
		m_written++;
	}

	long long written() const { return m_written; }

private:
	void flushBlock() {
		fputc(m_blockLen, m_file);
		fwrite(m_block, 1, m_blockLen, m_file);
		m_written += m_blockLen + 1;
		m_blockLen = 0;
	}

	FILE* m_file;
	unsigned char m_block[256] = {0};
	int m_blockLen = 0;
	uint32_t m_acc = 0;
	int m_bits = 0;
	long long m_written = 0;
};

long long lzwEncode(FILE* f, const unsigned char* data, size_t len) {
	const int minCodeSize = 8;
	const int clearCode = 1 << minCodeSize;		// 256
	const int endCode = clearCode + 1;		// 257

	fputc(minCodeSize, f);
	LzwOut out(f);

	std::unordered_map<uint32_t, int> dict;
	dict.reserve(8192);
	int next = endCode + 1;
	int codeWidth = minCodeSize + 1;

	out.putCode(clearCode, codeWidth);
	if (!len) {
		out.putCode(endCode, codeWidth);
		out.finish();
		return out.written() + 1;
	}

	int prefix = data[0];
	for (size_t i = 1; i < len; i++) {
		const unsigned char c = data[i];
		const uint32_t key = ((uint32_t)prefix << 8) | c;
		auto it = dict.find(key);
		if (it != dict.end()) {
			prefix = it->second;
			continue;
		}
		out.putCode(prefix, codeWidth);
		if (next < 4096) {
			dict[key] = next++;
			if (next > (1 << codeWidth) && codeWidth < 12) codeWidth++;
		} else {
			out.putCode(clearCode, codeWidth);
			dict.clear();
			next = endCode + 1;
			codeWidth = minCodeSize + 1;
		}
		prefix = c;
	}
	out.putCode(prefix, codeWidth);
	out.putCode(endCode, codeWidth);
	out.finish();
	return out.written() + 1;
}

void put16le(FILE* f, int v) {
	fputc(v & 0xff, f);
	fputc((v >> 8) & 0xff, f);
}

} // namespace

GifWriter::~GifWriter() {
	close();
}

bool GifWriter::open(const std::string& path, int width, int height,
		     const uint32_t* palette, int paletteSize, int loopCount, std::string& err) {
	close();
	if (width <= 0 || height <= 0) { err = "empty frame"; return false; }
	if (width > 65535 || height > 65535) { err = "frame too large for GIF"; return false; }

	m_file = fopen(path.c_str(), "wb");
	if (!m_file) { err = "can't write '" + path + "'"; return false; }
	m_width = width;
	m_height = height;
	m_bytes = 0;

	// the emulator's palette becomes the global colour table, so colours are
	// exact rather than quantised
	if (paletteSize < 1) paletteSize = 1;
	if (paletteSize > 256) paletteSize = 256;
	m_paletteSize = paletteSize;
	memset(m_palette, 0, sizeof(m_palette));
	for (int i = 0; i < paletteSize; i++) {
		const uint32_t c = palette[i];		// stored as R | G<<8 | B<<16 | A<<24
		m_palette[i * 3 + 0] = (unsigned char)(c & 0xff);
		m_palette[i * 3 + 1] = (unsigned char)((c >> 8) & 0xff);
		m_palette[i * 3 + 2] = (unsigned char)((c >> 16) & 0xff);
	}

	fwrite("GIF89a", 1, 6, m_file);
	put16le(m_file, width);
	put16le(m_file, height);
	fputc(0xf7, m_file);		// global table, 8 bits/pixel, 256 entries
	fputc(0x00, m_file);		// background colour index
	fputc(0x00, m_file);		// pixel aspect ratio
	fwrite(m_palette, 1, 256 * 3, m_file);

	// loop forever (or loopCount times)
	fputc(0x21, m_file); fputc(0xff, m_file); fputc(0x0b, m_file);
	fwrite("NETSCAPE2.0", 1, 11, m_file);
	fputc(0x03, m_file); fputc(0x01, m_file);
	put16le(m_file, loopCount < 0 ? 0 : loopCount);
	fputc(0x00, m_file);

	m_indexed.resize((size_t)width * height);
	return true;
}

int GifWriter::indexOf(unsigned char r, unsigned char g, unsigned char b) {
	// exact match first: ULA output only ever uses palette entries
	for (int i = 0; i < m_paletteSize; i++) {
		if (m_palette[i * 3] == r && m_palette[i * 3 + 1] == g && m_palette[i * 3 + 2] == b)
			return i;
	}
	int best = 0, bestDist = 1 << 30;
	for (int i = 0; i < m_paletteSize; i++) {
		const int dr = (int)r - m_palette[i * 3];
		const int dg = (int)g - m_palette[i * 3 + 1];
		const int db = (int)b - m_palette[i * 3 + 2];
		const int d = dr * dr + dg * dg + db * db;
		if (d < bestDist) { bestDist = d; best = i; }
	}
	return best;
}

bool GifWriter::addFrame(const unsigned char* rgba, int delayCs, std::string& err) {
	if (!m_file) { err = "recorder is not open"; return false; }
	if (delayCs < 1) delayCs = 1;

	// small colour cache: a frame has a handful of distinct colours
	std::unordered_map<uint32_t, int> cache;
	const size_t pixels = (size_t)m_width * m_height;
	for (size_t i = 0; i < pixels; i++) {
		const unsigned char r = rgba[i * 4 + 0];
		const unsigned char g = rgba[i * 4 + 1];
		const unsigned char b = rgba[i * 4 + 2];
		const uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
		auto it = cache.find(key);
		int idx;
		if (it != cache.end()) {
			idx = it->second;
		} else {
			idx = indexOf(r, g, b);
			cache[key] = idx;
		}
		m_indexed[i] = (unsigned char)idx;
	}

	fputc(0x21, m_file); fputc(0xf9, m_file); fputc(0x04, m_file);
	fputc(0x00, m_file);			// no disposal, no transparency
	put16le(m_file, delayCs);
	fputc(0x00, m_file);			// transparent index (unused)
	fputc(0x00, m_file);

	fputc(0x2c, m_file);			// image descriptor
	put16le(m_file, 0);
	put16le(m_file, 0);
	put16le(m_file, m_width);
	put16le(m_file, m_height);
	fputc(0x00, m_file);			// no local table, not interlaced

	m_bytes += lzwEncode(m_file, m_indexed.data(), m_indexed.size()) + 18;
	if (ferror(m_file)) { err = "write error"; return false; }
	return true;
}

bool GifWriter::close() {
	if (!m_file) return true;
	fputc(0x3b, m_file);			// trailer
	bool ok = !ferror(m_file);
	fclose(m_file);
	m_file = nullptr;
	return ok;
}

// ---------------------------------------------------------------- ffmpeg

static std::string quote(const std::string& s) {
#ifdef _WIN32
	return "\"" + s + "\"";
#else
	std::string out = "'";
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	return out + "'";
#endif
}

FfmpegWriter::~FfmpegWriter() {
	close();
}

bool FfmpegWriter::available() {
#ifdef _WIN32
	return system("ffmpeg -version >NUL 2>&1") == 0;
#else
	return system("ffmpeg -version >/dev/null 2>&1") == 0;
#endif
}

bool FfmpegWriter::open(const std::string& path, int width, int height, int fps, std::string& err) {
	close();
	char cmd[1024];
	// yuv420p needs even dimensions; crop rather than rescale so pixels stay sharp
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -y -loglevel error -f rawvideo -pixel_format rgba -video_size %dx%d "
		 "-framerate %d -i - -vf \"crop=trunc(iw/2)*2:trunc(ih/2)*2\" -pix_fmt yuv420p "
		 "-crf 20 %s",
		 width, height, fps, quote(path).c_str());
	m_pipe = XSP_POPEN(cmd, XSP_PIPE_WRITE);
	if (!m_pipe) { err = "can't start ffmpeg"; return false; }
	return true;
}

bool FfmpegWriter::addFrame(const unsigned char* rgba, size_t bytes, std::string& err) {
	if (!m_pipe) { err = "encoder is not open"; return false; }
	if (fwrite(rgba, 1, bytes, m_pipe) != bytes) { err = "ffmpeg stopped reading frames"; return false; }
	return true;
}

bool FfmpegWriter::close() {
	if (!m_pipe) return true;
	int rc = XSP_PCLOSE(m_pipe);
	m_pipe = nullptr;
	return rc == 0;
}

bool FfmpegWriter::mux(const std::string& videoPath, const std::string& wavPath,
		       const std::string& outPath, std::string& err) {
	char cmd[1536];
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -y -loglevel error -i %s -i %s -c:v copy -shortest %s",
		 quote(videoPath).c_str(), quote(wavPath).c_str(), quote(outPath).c_str());
	if (system(cmd) != 0) { err = "ffmpeg failed to add the audio track"; return false; }
	return true;
}

} // namespace record
} // namespace xsp
