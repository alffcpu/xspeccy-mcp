// Screen recording. A ZX screen is a handful of colours out of a 256-entry
// palette, which is exactly what GIF was made for - so the default recorder is
// built in and needs nothing installed. For MP4/WebM (and sound) we hand raw
// frames to ffmpeg if it is available.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "spectrum.h"
}

namespace xsp {
namespace record {

// Streams frames straight to disk, so a long recording costs no more memory
// than a short one.
class GifWriter {
public:
	~GifWriter();

	// palette: up to 256 RGBA entries, normally comp->vid->pal
	bool open(const std::string& path, int width, int height,
		  const uint32_t* palette, int paletteSize, int loopCount, std::string& err);
	bool addFrame(const unsigned char* rgba, int delayCentiseconds, std::string& err);
	bool close();
	long long bytes() const { return m_bytes; }

private:
	int indexOf(unsigned char r, unsigned char g, unsigned char b);

	FILE* m_file = nullptr;
	int m_width = 0, m_height = 0;
	int m_paletteSize = 0;
	unsigned char m_palette[256 * 3] = {0};
	std::vector<unsigned char> m_indexed;
	long long m_bytes = 0;
};

// A pipe into ffmpeg reading raw RGBA frames on stdin.
class FfmpegWriter {
public:
	~FfmpegWriter();

	static bool available();
	bool open(const std::string& path, int width, int height, int fps, std::string& err);
	bool addFrame(const unsigned char* rgba, size_t bytes, std::string& err);
	bool close();

	// Replaces `videoPath` with a copy carrying `wavPath` as its audio track.
	static bool mux(const std::string& videoPath, const std::string& wavPath,
			const std::string& outPath, std::string& err);

private:
	FILE* m_pipe = nullptr;
};

} // namespace record
} // namespace xsp
