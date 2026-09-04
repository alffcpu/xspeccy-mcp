// The picture: what is on screen, and recording it.
#include "tools/common.h"

#include <cstdio>

namespace xsp {
namespace tools {

// Screenshots and recordings are numbered so that a caller who does not name a
// file still gets a new one each time rather than overwriting the last.
static int g_shotCounter = 0;
static int g_videoCounter = 0;

void registerVideoTools() {
	tool("screenshot",
	     "Write the last completed frame to a PNG file and return its path - the ground truth of "
	     "what is on screen. Open the file to look at it. border=false crops to the 256x192 paper "
	     "area, scale enlarges by pixel doubling.\n"
	     "blend is for flickering pictures. A gigascreen, or any effect that alternates images "
	     "faster than the eye separates them, is meant to be seen as one picture whose colours "
	     "are not in the palette at all; a single frame is one half of that and looks like "
	     "neither. blend:2 averages the last two completed frames, blend:3 the last three, and "
	     "the result is the colour a person watching the screen sees. It reads the frame history "
	     "and changes nothing about the machine.",
	     json{{"properties", {
		     {"path", {{"type", "string"}, {"description", "output file; default: a temp file"}}},
		     {"border", {{"type", "boolean"}, {"description", "include the border, default true"}}},
		     {"scale", {{"type", "integer"}, {"description", "1..8, default 1"}}},
		     {"blend", {{"description", "frames to average: 2 for a gigascreen, 3 for a "
				 "three-frame effect, 1 for the raw frame (default)"}}}
	     }}},
	     [](const json& a) {
		     bool border = argBool(a, "border", true);
		     int scale = argNumOr(a, "scale", 1);
		     const int blend = argBlend(a);
		     std::string path = argStr(a, "path");
		     if (path.empty()) {
			     std::string dir = xsp::platform::join(xsp::platform::tempDir(), "xspeccy-mcp");
			     xsp::platform::makeDir(dir);
			     char buf[32];
			     snprintf(buf, sizeof(buf), "shot-%03d.png", ++g_shotCounter);
			     path = xsp::platform::join(dir, buf);
		     }
		     xsp::video::BlendInfo bi;
		     auto shot = xsp::video::capture(mach().comp(), border, scale, blend, &bi);
		     if (shot.rgba.empty()) throw std::runtime_error("no frame captured yet - run some frames first");
		     std::string err;
		     if (!xsp::writePng(path, shot.rgba.data(), shot.width, shot.height, err))
			     throw std::runtime_error(err);
		     json res{{"path", path}, {"width", shot.width}, {"height", shot.height},
			      {"border", border}, {"scale", scale}};
		     if (bi.frames > 1) res["blend"] = blendJson(bi);
		     return res;
	     });

	tool("record_video",
	     "Record the screen while the machine runs. Default is an animated GIF, which needs "
	     "nothing installed and reproduces the ZX palette exactly. Give the path an .mp4/.webm "
	     "extension to encode with ffmpeg instead, which also lets the recording carry the "
	     "machine's real sound, up to a minute of it - past that the sound stops and the "
	     "picture is cut to match, and the answer says so. Length is up to you: 100 frames is "
	     "two seconds, 15000 is five "
	     "minutes - frames are written as they are produced, so long recordings cost no extra "
	     "memory. Use every_nth to halve or quarter the frame rate for smaller files, or "
	     "every_nth:\"auto\" to have it match the effect's own update rate. skip_until winds "
	     "past the precalculation first, so the recording does not open on a black screen.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "emulated frames to record, default 100 (2 s)"}}},
		     {"path", {{"type", "string"}, {"description", "output file; extension picks the format, default .gif"}}},
		     {"every_nth", {{"description", "record every Nth frame, 1..1000, default 1; \"auto\" measures "
				    "the effect's quantum and uses that, so no frame is a duplicate"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before recording - e.g. the main loop, "
				     "to skip the precalculation"}}},
		     {"skip_max_instructions", {{"type", "integer"}, {"description",
				     "budget for the skip itself, default 200000000"}}},
		     {"border", {{"type", "boolean"}, {"description", "include the border, default true"}}},
		     {"scale", {{"type", "integer"}, {"description", "1..8, default 1"}}},
		     {"audio", {{"type", "boolean"}, {"description", "add the sound track (mp4/webm only), default true"}}},
		     {"blend", {{"description", "average this many frames into each recorded one, so a "
				 "gigascreen records as the picture it is meant to be instead of a "
				 "flicker; 2 for a gigascreen, 3 for a three-frame effect"}}}
	     }}},
	     [](const json& a) {
		     const int frames = argNumRange(a, "frames", 100, 1, 60000);	// 60000 is ~20 minutes
		     json skipped = skipUntil(a);
		     // An effect that needs N interrupts per frame draws the same picture
		     // N times running; recording all of them makes the file N times
		     // bigger and carries no extra pixel. The quantum is measurable, so
		     // measure it rather than asking the caller to know it.
		     json autoNth;
		     int nth = 1;
		     if (a.contains("every_nth") && a["every_nth"].is_string() &&
			 xsp::trim(a["every_nth"].get<std::string>()) == "auto") {
			     Timing t0 = timing(mach().comp());
			     const long long budget = 20000000;
			     long long worst = 0;
			     int measured = 0;
			     if (mach().frameCost(-1, budget, false).complete) {
				     for (int i = 0; i < 2; i++) {
					     xsp::FrameCost fc = mach().frameCost(-1, budget, false);
					     if (!fc.complete) break;
					     const long long w = mach().comp()->nsPerTick > 0
						     ? fc.work_ns / mach().comp()->nsPerTick : 0;
					     if (w > worst) worst = w;
					     measured++;
				     }
			     }
			     if (measured && t0.tPerFrame > 0) {
				     nth = (int)((worst + t0.tPerFrame - 1) / t0.tPerFrame);
				     if (nth < 1) nth = 1;
				     autoNth = json{{"every_nth", nth}, {"frame_t_states", worst},
						    {"t_states_per_frame", t0.tPerFrame},
						    {"frames_measured", measured}};
			     } else {
				     autoNth = json{{"every_nth", 1},
						    {"note", "no HALT boundary found, so the effect's "
							     "rate could not be measured - recording "
							     "every frame"}};
			     }
		     } else {
			     // Bounded at both ends. Below 1 there is no such thing as
			     // recording every zeroth frame; above this the output rate
			     // falls under a frame a second, which ffmpeg rounds to zero
			     // and which overflows the GIF's 16-bit centisecond delay -
			     // both of which produce a file rather than an error.
			     nth = argNumRange(a, "every_nth", 1, 1, 1000);
		     }
		     if (nth < 1) nth = 1;
		     const bool border = argBool(a, "border", true);
		     const int scale = argNumOr(a, "scale", 1);
		     const int blend = argBlend(a);

		     std::string path = argStr(a, "path");
		     if (path.empty()) {
			     std::string dir = xsp::platform::join(xsp::platform::tempDir(), "xspeccy-mcp");
			     xsp::platform::makeDir(dir);
			     char buf[32];
			     snprintf(buf, sizeof(buf), "video-%03d.gif", ++g_videoCounter);
			     path = xsp::platform::join(dir, buf);
		     }
		     std::string ext;
		     size_t dot = path.find_last_of('.');
		     if (dot != std::string::npos) ext = path.substr(dot + 1);
		     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
		     const bool gif = (ext == "gif" || ext.empty());
		     if (!gif && !xsp::record::FfmpegWriter::available())
			     throw std::runtime_error("ffmpeg is needed for ." + ext + " - use a .gif path instead");

		     Computer* c = mach().comp();
		     // the rate the machine really runs at, so playback matches wall-clock
		     const double fps = timing(c).fps > 0 ? timing(c).fps : 50.0;
		     const double outFps = fps / nth;
		     const bool withAudio = !gif && argBool(a, "audio", true);

		     // first frame decides the geometry, and with blending the palette too
		     xsp::video::BlendInfo bi;
		     auto shot = xsp::video::capture(c, border, scale, blend, &bi);
		     if (shot.rgba.empty()) throw std::runtime_error("no frame to record yet - run some frames first");

		     xsp::record::GifWriter gifw;
		     xsp::record::FfmpegWriter ff;
		     std::string err;
		     std::string videoPath = path;
		     std::string tmpVideo, tmpWav, audioNote;
		     // The intermediate video and its soundtrack exist only to be muxed
		     // together; leaving them behind fills the user's temporary
		     // directory with copies of every recording ever made. Declared
		     // here so that the names can be handed over the moment they are
		     // chosen, which is before anything that can throw.
		     struct ScratchFiles {
			     std::string video, wav;
			     ~ScratchFiles() {
				     if (!video.empty()) std::remove(video.c_str());
				     if (!wav.empty()) std::remove(wav.c_str());
			     }
		     } scratch;
		     bool paletteExact = true;
		     if (gif) {
			     std::vector<uint32_t> mixed;
			     const uint32_t* pal = c->vid->pal;
			     int palSize = 256;
			     if (bi.on) {
				     mixed = record::picturePalette(shot.rgba, paletteExact);
				     pal = mixed.data();
				     palSize = (int)mixed.size();
			     }
			     if (!gifw.open(path, shot.width, shot.height, pal, palSize, 0, err))
				     throw std::runtime_error(err);
		     } else {
			     if (withAudio) {
				     std::string dir = xsp::platform::join(xsp::platform::tempDir(), "xspeccy-mcp");
				     xsp::platform::makeDir(dir);
				     // Named for this process and this recording. They used
				     // to be called rec-video and rec-audio, so two servers
				     // sharing a temporary directory wrote over each other
				     // and neither found out.
				     char stem[64];
				     snprintf(stem, sizeof(stem), "rec-%d-%d",
					      xsp::platform::processId(), g_videoCounter);
				     tmpVideo = xsp::platform::join(dir, std::string(stem) + "." + ext);
				     tmpWav = xsp::platform::join(dir, std::string(stem) + ".wav");
				     videoPath = tmpVideo;
				     // Named for deletion now rather than after the frame
				     // loop: a throw inside the loop used to leave both
				     // behind, which is the case that leaves them behind
				     // most often.
				     scratch.video = tmpVideo;
				     scratch.wav = tmpWav;
			     }
			     if (!ff.open(videoPath, shot.width, shot.height, (int)(outFps + 0.5), err))
				     throw std::runtime_error(err);
		     }

		     // However this ends. A throw out of the loop below used to leave
		     // the capture running, and every later execution went on filling
		     // it until something started a new one.
		     struct CaptureGuard {
			     xsp::audio::Capture* cap = nullptr;
			     ~CaptureGuard() { if (cap) cap->stop(); }
		     } captureGuard;
		     if (withAudio) {
			     mach().audio().start(44100, true, false);
			     captureGuard.cap = &mach().audio();
		     }


		     const int delayCs = (int)(100.0 / outFps + 0.5);
		     int written = 0;
		     for (int i = 0; i < frames; i++) {
			     mach().runFrames(1);
			     if (i % nth) continue;
			     auto f = xsp::video::capture(c, border, scale, blend);
			     if (f.width != shot.width || f.height != shot.height) continue;	// geometry changed mid-recording
			     bool ok = gif ? gifw.addFrame(f.rgba.data(), delayCs, err)
					   : ff.addFrame(f.rgba.data(), f.rgba.size(), err);
			     if (!ok) throw std::runtime_error(err);
			     written++;
		     }

		     // The writers check their own fclose; the verdict is only worth
		     // checking if the caller looks at it. A close that fails here has
		     // written a file that exists and is short, which the size check
		     // below cannot tell from a complete one.
		     if (gif) {
			     if (!gifw.close())
				     throw std::runtime_error("the recording could not be finished: "
							      "writing '" + path + "' failed");
		     } else {
			     if (!ff.close())
				     throw std::runtime_error("ffmpeg did not finish cleanly");
			     if (withAudio) {
				     auto& cap = mach().audio();
				     cap.stop();
				     // ffmpeg is told -shortest, so a soundtrack that
				     // stopped early takes the picture with it. Saying so
				     // beats handing back a file that is quietly half the
				     // length the caller asked for.
				     if (cap.pcm_truncated)
					     audioNote = "the sound ran out before the picture did, and "
							 "the two are cut to the shorter of them. Record "
							 "fewer frames for a complete one.";
				     if (xsp::audio::writeWav(tmpWav, cap, err)) {
					     if (!xsp::record::FfmpegWriter::mux(tmpVideo, tmpWav, path, err))
						     throw std::runtime_error(err);
				     } else {
					     throw std::runtime_error("audio: " + err);
				     }
			     }
		     }
		     if (withAudio) mach().audio().stop();

		     long long size = 0;
		     if (FILE* f = fopen(path.c_str(), "rb")) {
			     fseek(f, 0, SEEK_END);
			     size = ftell(f);
			     fclose(f);
		     }
		     if (!size) throw std::runtime_error("nothing was written to '" + path + "'");

		     json res{
			     {"path", path},
			     {"format", gif ? "gif" : ext},
			     {"frames_recorded", written},
			     {"frames_emulated", frames},
			     {"every_nth", nth},
			     {"fps", outFps},
			     {"duration_s", frames / fps},
			     {"width", shot.width},
			     {"height", shot.height},
			     {"bytes", size},
			     {"audio", withAudio}
		     };
		     if (!audioNote.empty()) res["audio_note"] = audioNote;
		     if (!skipped.is_null()) res["skipped_to"] = skipped;
		     if (!autoNth.is_null()) res["every_nth_auto"] = autoNth;
		     if (bi.frames > 1) {
			     res["blend"] = blendJson(bi);
			     if (gif && !paletteExact)
				     res["blend"]["palette"] = "the blend made more than 256 colours; "
							       "the 256 most used are exact and the rest "
							       "are nearest matches. Record .mp4 for all of them.";
		     }
		     return res;
	     });

	tool("screen_text",
	     "Decode the ZX screen into 32x24 text by matching each cell against the ROM font. Cheap "
	     "way to read a BASIC listing or a menu. Graphics come out as '?' - for anything visual "
	     "use screenshot, which shows the real pixels. Reads the screen page the ULA is actually "
	     "displaying, and says which one that was; `page` reads a given RAM page instead.",
	     json{{"properties", {
		     {"page", {{"type", "integer"}, {"description",
			      "RAM page to read, 0-255. Default: the page on air ($7FFD bit 3, so 5 or 7)"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = mach().comp();
		     const int req = argNumRange(a, "page", -1, 0, 255);
		     const int page = (req < 0) ? xsp::video::displayedPage(c) : req;
		     return json{{"screen", xsp::video::screenText(c, page)},
				 {"page", page},
				 {"page_on_air", xsp::video::displayedPage(c)}};
	     });

	tool("screen_attrs",
	     "The 32x24 attribute grid as hex (ink/paper/bright/flash per cell). Cheaper to read "
	     "than a screenshot when the question is about colour rather than shape, and unlike a "
	     "PNG it can be compared in the reply itself. Reads the screen page the ULA is actually "
	     "displaying, and says which one that was; `page` reads a given RAM page instead. That "
	     "matters for anything that flips screens between frames: the attributes of the page "
	     "nobody is looking at describe a picture that is not on the screen.",
	     json{{"properties", {
		     {"page", {{"type", "integer"}, {"description",
			      "RAM page to read, 0-255. Default: the page on air ($7FFD bit 3, so 5 or 7)"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = mach().comp();
		     const int req = argNumRange(a, "page", -1, 0, 255);
		     const int page = (req < 0) ? xsp::video::displayedPage(c) : req;
		     return json{{"attributes", xsp::video::screenAttrs(c, page)},
				 {"page", page},
				 {"page_on_air", xsp::video::displayedPage(c)}};
	     });

	tool("screen_digest",
	     "A short hash of the screen, per frame - the cheap way to prove an optimisation did not "
	     "change the picture. Run it before a change and after it and compare the lists; identical "
	     "digests mean the output is identical byte for byte. It hashes screen memory in the RAM "
	     "banks rather than the visible frame, and by default both screen banks (5 and 7), because "
	     "double-buffered code writes only one of them per frame and hashing just one flip-flops "
	     "between matching and not. A frame is the code between two HALTs by default (see `sync`), "
	     "which is the effect's own frame even when it overruns the interrupt. Give `from` and "
	     "`to` to hash a stretch of memory instead of the screen - a precalculated table, a "
	     "buffer, anything the picture does not show.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "frames to run and hash, default 1"}}},
		     {"scope", {{"type", "string"}, {"description", "attrs (default, 768 bytes/bank) | screen (pixels+attrs)"}}},
		     {"banks", {{"type", "array"}, {"description", "RAM banks to hash, default [5,7] (or [5] on a 48K)"},
				{"items", {{"type", "integer"}}}}},
		     {"sync", {{"type", "string"}, {"description", "halt (default) | frame | an address/label to stop at"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before hashing anything - e.g. the main "
				     "loop, to get past the precalculation"}}},
		     {"skip_max_instructions", {{"type", "integer"}, {"description",
				     "budget for the skip itself, default 200000000"}}},
		     {"from", {{"type", "string"}, {"description",
			       "hash this CPU address range instead of the screen (with `to`) - a "
			       "table, a buffer, anything the picture does not show"}}},
		     {"to", {{"type", "string"}, {"description", "last address of the range, inclusive"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "per-frame budget, default 20000000"}}}
	     }}},
	     [](const json& a) {
		     const int frames = argNumRange(a, "frames", 1, 1, 10000);
		     json skipped = skipUntil(a);

		     // an explicit range means "hash this memory", and then the screen
		     // arguments have nothing to describe
		     int from = 0, to = 0;
		     const bool hasFrom = argAddr(a, "from", from);
		     const bool hasTo = argAddr(a, "to", to);
		     if (hasFrom != hasTo)
			     throw std::runtime_error("give both from and to, or neither");
		     const bool byRange = hasFrom;
		     if (byRange && (a.contains("banks") || a.contains("scope")))
			     throw std::runtime_error("from/to hash memory directly: banks and scope "
						      "describe the screen and cannot apply");

		     std::string scope = argStr(a, "scope", "attrs");
		     if (scope != "attrs" && scope != "screen")
			     throw std::runtime_error("scope must be attrs or screen");
		     std::vector<int> banks;
		     if (a.contains("banks") && a["banks"].is_array()) {
			     for (const auto& b : a["banks"]) banks.push_back(b.get<int>());
			     if (banks.empty()) throw std::runtime_error("banks[] is empty");
		     } else if (!byRange) {
			     banks.push_back(5);
			     if (mach().memoryKb() >= 128) banks.push_back(7);
		     }
		     SyncSpec sync = argSync(a);
		     const long long budget = argNumRange(a, "max_instructions", 20000000,
							  1, (int)kMaxInstructions);

		     json list = json::array();
		     std::string err;
		     bool truncated = false;
		     for (int i = 0; i < frames; i++) {
			     if (sync.byInterrupt) {
				     mach().runFrames(1);
			     } else {
				     xsp::FrameCost fc = mach().frameCost(sync.pc, budget);
				     if (!fc.complete) { truncated = true; }
			     }
			     std::string d = byRange
				     ? xsp::video::memoryDigest(mach().comp(), from, to, err)
				     : xsp::video::screenDigest(mach().comp(), banks,
								scope == "screen", err);
			     if (d.empty()) throw std::runtime_error(err);
			     list.push_back({{"frame", i}, {"digest", d}});
			     if (truncated) break;
		     }
		     json res{{"sync", sync.text}, {"frames", (int)list.size()}, {"digests", list}};
		     if (byRange) {
			     res["scope"] = "memory";
			     res["from"] = from;
			     res["from_hex"] = hex16(from);
			     res["to"] = to;
			     res["to_hex"] = hex16(to);
			     res["bytes"] = to - from + 1;
		     } else {
			     res["scope"] = scope;
			     res["banks"] = banks;
		     }
		     if (!skipped.is_null()) res["skipped_to"] = skipped;
		     if (truncated)
			     res["warning"] = "no frame boundary within max_instructions - the digest "
					      "list is short; check `sync`";
		     return res;
	     });

	tool("frame_digest",
	     "A short hash of the frame as the ULA actually drew it - the picture, not the memory "
	     "behind it. This is the one to use for multicolour, and it answers a different question "
	     "from screen_digest. screen_digest hashes screen memory; the visible picture is a "
	     "function of that memory AND of when the bank is switched relative to the beam, so raster "
	     "code whose timing has drifted paints a different screen out of byte-identical data and "
	     "screen_digest cannot see it move. If your data hashes stable and the picture still looks "
	     "wrong, that is not a paradox - it is this. `lines` hashes every scanline separately and "
	     "reports which line ranges differ from the previous frame, which turns \"some frames are "
	     "wrong\" into a line number.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "frames to run and hash, default 1"}}},
		     {"border", {{"type", "boolean"}, {"description", "include the border, default true"}}},
		     {"lines", {{"type", "boolean"}, {"description",
				"also hash each scanline and report the ranges that changed"}}},
		     {"sync", {{"type", "string"}, {"description", "halt | frame (default) | an address/label"}}},
		     {"skip_until", {{"type", "string"}, {"description",
				     "run to this address/label before hashing - e.g. past the precalculation"}}},
		     {"skip_max_instructions", {{"type", "integer"}, {"description",
				     "budget for the skip itself, default 200000000"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "per-frame budget, default 20000000"}}},
		     {"blend", {{"description", "hash the average of this many frames instead of one, "
				 "which is what to compare when the picture flickers on purpose"}}}
	     }}},
	     [](const json& a) {
		     int frames = argNumRange(a, "frames", 1, 1, 10000);
		     const bool border = argBool(a, "border", true);
		     const bool perLine = argBool(a, "lines", false);
		     const int blend = argBlend(a);
		     json skipped = skipUntil(a);
		     // A hardware frame by default, unlike screen_digest: the picture is
		     // made by the ULA, and its unit is the interrupt rather than wherever
		     // the effect happens to call its own frame boundary.
		     SyncSpec sync = argSync(a);
		     if (!a.is_object() || !a.contains("sync") || a["sync"].is_null()) {
			     sync.byInterrupt = true;
			     sync.pc = -1;
			     sync.text = "frame";
		     }
		     const long long budget = argNumRange(a, "max_instructions", 20000000,
							  1, (int)kMaxInstructions);

		     json list = json::array();
		     std::vector<std::string> prevLines;
		     bool truncated = false;
		     int width = 0, height = 0;
		     xsp::video::BlendInfo blendInfo;
		     std::map<std::string, int> seen;	// digest -> first frame that had it
		     for (int i = 0; i < frames; i++) {
			     if (sync.byInterrupt) {
				     mach().runFrames(1);
			     } else {
				     xsp::FrameCost fc = mach().frameCost(sync.pc, budget);
				     if (!fc.complete) truncated = true;
			     }
			     xsp::video::BlendInfo bi;
			     xsp::video::FrameHash fh =
				     xsp::video::frameDigest(mach().comp(), border, perLine, blend, &bi);
			     blendInfo = bi;
			     if (fh.digest.empty())
				     throw std::runtime_error("no frame buffer to hash yet - run first");
			     width = fh.width;
			     height = fh.height;
			     json e{{"frame", i}, {"digest", fh.digest}};
			     auto it = seen.find(fh.digest);
			     if (it == seen.end()) seen.emplace(fh.digest, i);
			     else e["same_as_frame"] = it->second;
			     if (perLine) {
				     // Ranges rather than 300 hashes: what is wanted is "which
				     // part of the screen moved", and a list of line numbers is
				     // the same answer spelled out at length.
				     json ranges = json::array();
				     if (!prevLines.empty()) {
					     int run = -1;
					     for (size_t y = 0; y < fh.lines.size(); y++) {
						     const bool diff = y >= prevLines.size() ||
								       fh.lines[y] != prevLines[y];
						     if (diff && run < 0) run = (int)y;
						     if (!diff && run >= 0) {
							     ranges.push_back({run, (int)y - 1});
							     run = -1;
						     }
					     }
					     if (run >= 0)
						     ranges.push_back({run, (int)fh.lines.size() - 1});
				     }
				     e["changed_lines"] = ranges;
				     e["changed_line_count"] = [&]() {
					     int n = 0;
					     for (const auto& rg : ranges)
						     n += rg[1].get<int>() - rg[0].get<int>() + 1;
					     return n;
				     }();
				     prevLines = fh.lines;
			     }
			     list.push_back(e);
			     if (truncated) break;
		     }
		     json res{{"sync", sync.text}, {"frames", (int)list.size()},
			      {"scope", border ? "frame with border" : "paper only"},
			      {"width", width}, {"height", height},
			      {"unique_digests", (int)seen.size()},
			      {"digests", list}};
		     if (blendInfo.frames > 1) res["blend"] = blendJson(blendInfo);
		     if (!skipped.is_null()) res["skipped_to"] = skipped;
		     if (truncated)
			     res["warning"] = "no frame boundary within max_instructions - the digest "
					      "list is short; check `sync`";
		     return res;
	     });

	// -------------------------------------------------- symbols
}

} // namespace tools
} // namespace xsp
