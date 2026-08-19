// Video side of a headless machine: the globals xgui normally owns, frame
// capture to PNG, and reading the ZX screen as text.
#pragma once

#include <string>
#include <vector>

extern "C" {
#include "spectrum.h"
}

namespace xsp {
namespace video {

// Sets bytesPerLine/xstep/ystep/skips the way MainWin::updateWindow() and
// vid_upd_scale() do, for zoom 1 and no scaling filters. Must be called after
// every layout change or the frame buffer geometry goes stale.
void applyGeometry(Computer* comp);

struct Shot {
	int width = 0;
	int height = 0;
	std::vector<unsigned char> rgba;
};

// Grabs the last completed frame. `border=false` crops to the paper area.
Shot capture(Computer* comp, bool border, int scale);

// Decodes the 32x24 ZX text screen by matching each cell against the ROM font
// (CHARS system variable, falling back to $3C00). Unmatched cells become '?'.
std::string screenText(Computer* comp);

// Attribute grid as "ink,paper,bright,flash" rows - cheap way to see colour
// without looking at a picture.
std::string screenAttrs(Computer* comp);

// Short hash of the screen memory of the given RAM banks, so "did this change
// the picture?" costs one line instead of 768 bytes per frame. `pixels` adds
// the 6144-byte bitmap to the 768 attribute bytes. Banks are hashed in the
// order given; the result is the first 12 hex digits of the MD5, which makes
// it byte-compatible with the reference capture scripts ZX projects tend to
// grow. Returns "" and sets err if a bank is outside the machine's RAM.
std::string screenDigest(Computer* comp, const std::vector<int>& banks,
			 bool pixels, std::string& err);

// The same hash over an arbitrary CPU address range, for the data a screen
// digest cannot see: a table, a buffer, a sprite bank.
std::string memoryDigest(Computer* comp, int from, int to, std::string& err);

// Hash of the last completed frame as the ULA actually drew it, which is a
// different question from screenDigest() and the reason this exists.
//
// screenDigest() hashes screen memory. In multicolour the picture is a function
// of that memory AND of when the bank is switched relative to the beam, so the
// same bytes drawn a few T-states off produce a different screen while the
// memory hash does not move at all. A stable screenDigest() proves the data is
// stable and says nothing whatever about the raster. This one looks at the
// pixels that came out.
//
// `perLine` additionally hashes every scanline on its own, which is what turns
// "some frames are wrong" into "line 137 onwards is wrong".
struct FrameHash {
	std::string digest;			// the whole (cropped) frame
	std::vector<std::string> lines;		// per scanline, when perLine
	int width = 0;
	int height = 0;
};
FrameHash frameDigest(Computer* comp, bool border, bool perLine);

} // namespace video

// PNG writer (zlib, no external image library)
bool writePng(const std::string& path, const unsigned char* rgba,
	      int width, int height, std::string& err);

} // namespace xsp
