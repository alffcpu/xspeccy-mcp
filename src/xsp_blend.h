// Frame blending: seeing a flickering picture the way the eye sees it.
//
// A gigascreen or a flickering multicolour effect alternates two or three
// pictures at 50 Hz and is meant to be looked at as one. On a CRT the phosphor
// and the eye do the averaging, and the colours that come out are not in the
// palette at all - that is the entire point of the technique. A screenshot
// catches whichever of the alternating frames the machine happened to stop on,
// which is the one thing the effect was never meant to show, so an agent
// looking at it sees flicker artefacts instead of the intended colour.
//
// Xpeccy does this in its GUI (xcore/vfilters.cpp:scrMix, driven by the global
// `noflic`), but two things there are for a human at a monitor rather than for
// us: it blends destructively into the displayed buffer, and its adaptive modes
// guess which pixels are "really" alternating so a static picture stays sharp.
// We keep the raw frames and average them, which needs no guessing and cannot
// be wrong about a pixel.
//
// What is kept from upstream is the part that is not a matter of taste: the
// averaging happens in linear light, not on the sRGB bytes. Averaging encoded
// values gives a mix that is visibly too dark, which is what the reference on
// gigascreen colour (hype.retroscene.org/blog/graphics/808.html) is about.
#pragma once

#include <cstddef>
#include <vector>

namespace xsp {
namespace blend {

// Frames kept for blending. Three-frame effects need three; the extra room is
// for effects whose cycle is longer, and costs one memcpy per frame either way.
constexpr int kMaxHistory = 8;

// Defaults used when a call does not say otherwise, so "blend everything from
// now on" is one call to video_config rather than an argument on every tool.
struct Settings {
	int frames = 1;			// 1 = off (raw last frame), 2 = gigascreen, 3 = three-frame
	double gamma = 2.2;		// transfer used to get into linear light; 1 = average the bytes
	int history = 4;		// ring depth, 0 = record nothing
};
Settings& defaults();

// ---- the ring of completed frames -------------------------------------
// Raw frames, never blended in place: a digest and a screenshot of the same
// moment have to keep describing the same pixels.

void setDepth(int depth);		// clamped to 0..kMaxHistory; changing it clears
int depth();
int filled();				// frames actually recorded so far
void clear();				// after a reset, a load, or a geometry change
void push(const unsigned char* frame, size_t bytes);

// Up to `want` frames, newest first. Shorter when that many have not been
// recorded yet; empty when the ring is off or still cold.
std::vector<const unsigned char*> history(int want);

// How many of `src` differ from each other. An effect that repeats each picture
// for two interrupts blends two identical frames and looks unmixed, which is
// worth saying out loud rather than leaving the caller to wonder.
int distinctCount(const std::vector<const unsigned char*>& src, size_t bytes);

// What the recent frames are doing, which decides whether blending them is the
// right thing at all.
//
// Blending is for flicker: two pictures alternating, meant to be seen as one.
// An effect that simply animates also has different frames from one interrupt to
// the next, and averaging those is motion blur - the caller gets a smeared
// picture and no warning, because "the frames differ" is true of both. Telling
// them apart is cheap: under flicker a frame equals the one two before it.
enum class Pattern { Unknown, Still, Flicker, Animation };
Pattern pattern(size_t bytes);
const char* patternName(Pattern p);

// ---- the blend --------------------------------------------------------
//
// Averages one rectangle of `src` (newest first, all with the same stride) into
// a tightly packed RGBA buffer of w*h pixels. One source frame is a plain copy,
// which is the raw path.
void mixRect(const std::vector<const unsigned char*>& src, double gamma,
	     int stride, int x0, int y0, int w, int h,
	     std::vector<unsigned char>& out);

} // namespace blend
} // namespace xsp
