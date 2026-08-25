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

// What a blended capture actually did, so a caller is never left guessing which
// picture it is looking at.
struct BlendInfo {
	bool on = false;
	int frames = 1;			// asked for
	int used = 1;			// available in the history, so actually averaged
	int distinct = 1;		// of those, how many differ from each other
	const char* pattern = "unknown";	// still / flicker / animation
	double gamma = 2.2;
};

// Grabs the last completed frame. `border=false` crops to the paper area.
//
// `blendFrames` averages that many of the most recent completed frames instead
// of returning the last one alone, which is how a flickering picture is seen as
// the one image it is meant to be. -1 takes the setting from blend::defaults(),
// 0 or 1 is the raw frame.
Shot capture(Computer* comp, bool border, int scale,
	     int blendFrames = -1, BlendInfo* info = nullptr);

// Copies the frame that just finished into the blend history. Called at every
// frame boundary; does nothing when the history is switched off.
void historyPush(Computer* comp);

// Drops the history. Anything that makes the frames on either side of it belong
// to different programs - a reset, a load, a geometry change - must call this,
// or a blend would average two unrelated pictures.
void historyClear();

// Which RAM page the ULA is reading its picture from right now. On a 128K or a
// Pentagon this is bit 3 of $7FFD: 5 or 7. Code that flips screens between
// frames changes this and nothing else, so it is the only honest answer to
// "which of the two screens is on air".
int displayedPage(Computer* comp);

// Reads one byte of a screen page the way the ULA itself does - see vid_mrd_cb
// in spectrum.c, which is ramData[MADR(page,adr) & ramMask]. The mask is what
// makes this right on a 48K too: there is no bank 5 in 64K of flat RAM, and the
// address wraps back onto the one screen the machine has.
int screenByte(Computer* comp, int page, int adr);

// Decodes the 32x24 ZX text screen by matching each cell against the ROM font
// (CHARS system variable, falling back to $3C00). Unmatched cells become '?'.
// `page` is a RAM page as above; pass displayedPage() for what is on air.
std::string screenText(Computer* comp, int page);

// Attribute grid as "ink,paper,bright,flash" rows - cheap way to see colour
// without looking at a picture. `page` as for screenText.
std::string screenAttrs(Computer* comp, int page);

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
FrameHash frameDigest(Computer* comp, bool border, bool perLine,
		      int blendFrames = -1, BlendInfo* info = nullptr);

} // namespace video

// PNG writer (zlib, no external image library)
bool writePng(const std::string& path, const unsigned char* rgba,
	      int width, int height, std::string& err);

} // namespace xsp
