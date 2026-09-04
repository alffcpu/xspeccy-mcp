// What the server says about sound, and the WAV it hands back. Neither can be
// checked by listening from a test, but both are arithmetic over a buffer, and
// the arithmetic is where the mistakes are: the core's mixer output is unsigned
// and not centred, so anything that treats it as a signed waveform reports a
// loud constant tone for silence.
#include "unit.h"
#include "xsp_audio.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xsp;
using namespace xsp::audio;

namespace {

// Fills a capture as the execution loop would, from a list of stereo samples
// given in the core's own unsigned units.
void feedSamples(Capture& cap, const std::vector<int>& left,
		 const std::vector<int>& right, long long ns) {
	cap.samples = left.size();
	cap.ns_total = ns;
	cap.sum_l = cap.sum_r = cap.sq_l = cap.sq_r = 0;
	cap.min_l = cap.min_r = 0x7fffffff;
	cap.max_l = cap.max_r = -0x7fffffff;
	for (size_t i = 0; i < left.size(); i++) {
		cap.sum_l += left[i];
		cap.sq_l += (long long)left[i] * left[i];
		if (left[i] < cap.min_l) cap.min_l = left[i];
		if (left[i] > cap.max_l) cap.max_l = left[i];
		cap.sum_r += right[i];
		cap.sq_r += (long long)right[i] * right[i];
		if (right[i] < cap.min_r) cap.min_r = right[i];
		if (right[i] > cap.max_r) cap.max_r = right[i];
		cap.pcm.push_back((int16_t)left[i]);
		cap.pcm.push_back((int16_t)right[i]);
	}
}

std::vector<unsigned char> readFile(const std::string& path) {
	std::vector<unsigned char> out;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return out;
	unsigned char buf[8192];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
	std::fclose(f);
	return out;
}

uint32_t le32(const std::vector<unsigned char>& v, size_t at) {
	return (uint32_t)v[at] | ((uint32_t)v[at + 1] << 8) |
	       ((uint32_t)v[at + 2] << 16) | ((uint32_t)v[at + 3] << 24);
}

int le16s(const std::vector<unsigned char>& v, size_t at) {
	return (int16_t)(uint16_t)(v[at] | (v[at + 1] << 8));
}

bool tagAt(const std::vector<unsigned char>& v, size_t at, const char* tag) {
	return v.size() >= at + 4 && std::memcmp(&v[at], tag, 4) == 0;
}

} // namespace

void test_audio() {
	unit::begin("audio");

	// ---- starting a capture -------------------------------------------------
	//
	// The rate is clamped rather than refused, because a caller asking for
	// 8 Hz wants sound and would otherwise get an error instead of a recording.
	{
		Capture cap;
		cap.start(44100, true, false);
		CHECK_EQ(cap.rate, 44100);
		CHECK(cap.on);
		CHECK(cap.keep_pcm);
		CHECK(!cap.watch);

		cap.start(10, true, false);
		CHECK_EQ(cap.rate, 4000);
		cap.start(999999, true, false);
		CHECK_EQ(cap.rate, 192000);

		// Starting again wipes what the previous capture left behind. A
		// second recording that inherits the first one's samples reports a
		// duration nobody asked for.
		cap.start(44100, true, true);
		cap.pcm.push_back(1);
		cap.samples = 5;
		cap.beeper_toggles = 3;
		cap.start(44100, true, false);
		CHECK(cap.pcm.empty());
		CHECK_EQ((int)cap.samples, 0);
		CHECK_EQ(cap.beeper_toggles, 0);
		CHECK(!cap.watch);
		CHECK_EQ(cap.ns_total, 0LL);

		cap.stop();
		CHECK(!cap.on);
	}

	// ---- the energy of one source -------------------------------------------
	//
	// rms() is the standard deviation, not the mean: a source sitting at a
	// constant level is inaudible however large that level is.
	{
		SourceEnergy quiet;
		CHECK_EQ(quiet.rms(), 0.0);		// nothing sampled at all

		SourceEnergy flat;
		for (int i = 0; i < 100; i++) { flat.sum += 5000; flat.sq += 5000LL * 5000; flat.n++; }
		CHECK(flat.rms() < 1e-6);

		SourceEnergy square;
		for (int i = 0; i < 100; i++) {
			const int v = (i % 2) ? 1100 : 900;	// mean 1000, swing 100
			square.sum += v;
			square.sq += (long long)v * v;
			square.n++;
		}
		CHECK(std::fabs(square.rms() - 100.0) < 1e-6);
	}

	// ---- summarizing nothing ------------------------------------------------
	//
	// A capture with no samples has to produce zeroes rather than a division by
	// the sample count.
	{
		Capture cap;
		cap.start(44100, true, false);
		Summary s = summarize(cap);
		CHECK_EQ(s.rate, 44100);
		CHECK_EQ((int)s.samples, 0);
		CHECK_EQ(s.duration_s, 0.0);
		CHECK(s.silent);
		CHECK_EQ(s.dominant_source, std::string("silence"));
		CHECK_EQ(s.rms, 0.0);
		CHECK_EQ(s.peak, 0);
		CHECK_EQ(s.dominant_hz, 0.0);
	}

	// ---- a constant level is silence ----------------------------------------
	//
	// This is the one that matters. The mixer's idle output is a large positive
	// constant, and a summary that measured the level rather than the movement
	// would call a silent machine loud.
	{
		Capture cap;
		cap.start(44100, true, false);
		std::vector<int> flat(1000, 30000);
		feedSamples(cap, flat, flat, 1000000000LL);	// one second
		Summary s = summarize(cap);
		CHECK_EQ((int)s.samples, 1000);
		CHECK(std::fabs(s.duration_s - 1.0) < 1e-9);
		CHECK(s.rms < 1e-6);
		CHECK_EQ(s.peak, 0);
		CHECK(s.silent);
		CHECK_EQ(s.raw_min, 30000);
		CHECK_EQ(s.raw_max, 30000);
	}

	// A square wave around that same constant is not silence, and its peak and
	// rms are both the swing rather than the level.
	{
		Capture cap;
		cap.start(44100, true, false);
		std::vector<int> wave;
		for (int i = 0; i < 1000; i++) wave.push_back(i % 2 ? 31000 : 29000);
		feedSamples(cap, wave, wave, 1000000000LL);
		cap.beeper_toggles = 500;
		cap.zero_crossings = 1000;
		Summary s = summarize(cap);
		CHECK(std::fabs(s.rms - 1000.0) < 1e-6);
		CHECK_EQ(s.peak, 1000);
		CHECK(!s.silent);
		// Two crossings per cycle, over one second.
		CHECK(std::fabs(s.dominant_hz - 500.0) < 1e-9);
	}

	// The silence threshold is small movement AND no beeper edge: a beeper
	// clicking quietly is still something happening, and saying "silent" would
	// send the caller looking in the wrong place.
	{
		Capture cap;
		cap.start(44100, true, false);
		std::vector<int> tiny;
		for (int i = 0; i < 100; i++) tiny.push_back(i % 2 ? 30001 : 30000);
		feedSamples(cap, tiny, tiny, 100000000LL);
		Summary quiet = summarize(cap);
		CHECK(quiet.peak < 8);
		CHECK(quiet.silent);

		cap.beeper_toggles = 1;
		Summary clicked = summarize(cap);
		CHECK(!clicked.silent);
	}

	// ---- which source is making the noise -----------------------------------
	{
		Capture cap;
		cap.start(44100, true, false);
		std::vector<int> wave;
		for (int i = 0; i < 200; i++) wave.push_back(i % 2 ? 31000 : 29000);
		feedSamples(cap, wave, wave, 200000000LL);

		auto load = [](SourceEnergy& e, int swing) {
			e = SourceEnergy();
			for (int i = 0; i < 200; i++) {
				const int v = (i % 2) ? 1000 + swing : 1000 - swing;
				e.sum += v;
				e.sq += (long long)v * v;
				e.n++;
			}
		};

		load(cap.beeper, 500);
		load(cap.ay, 100);
		load(cap.gs, 10);
		CHECK_EQ(summarize(cap).dominant_source, std::string("beeper"));

		load(cap.beeper, 10);
		load(cap.ay, 500);
		CHECK_EQ(summarize(cap).dominant_source, std::string("ay"));

		load(cap.ay, 0);
		load(cap.beeper, 0);
		load(cap.gs, 500);
		CHECK_EQ(summarize(cap).dominant_source, std::string("general_sound"));

		// Below the floor nothing is dominant, because naming a source for a
		// signal that quiet is a guess dressed up as a measurement.
		load(cap.beeper, 0);
		load(cap.ay, 0);
		load(cap.gs, 0);
		CHECK_EQ(summarize(cap).dominant_source, std::string("silence"));
	}

	// ---- the WAV file -------------------------------------------------------
	{
		unit::TempDir tmp("audio");

		// Nothing captured is a failure with a reason, not an empty file that
		// a player opens and plays for zero seconds.
		{
			Capture cap;
			cap.start(44100, false, false);
			std::string err;
			CHECK(!writeWav(tmp.reserve("empty.wav"), cap, err));
			CHECK(err.find("keep_pcm") != std::string::npos);
		}

		Capture cap;
		cap.start(22050, true, false);
		std::vector<int> left, right;
		for (int i = 0; i < 500; i++) {
			// Deliberately above 32767: the core's output is unsigned, and
			// the value is carried in an int16_t. Reading it back as signed
			// would turn the loudest half of the waveform inside out, which
			// sounds like distortion and measures like noise.
			left.push_back(i % 2 ? 40000 : 20000);
			right.push_back(i % 2 ? 20000 : 40000);
		}
		feedSamples(cap, left, right, 500000000LL);

		const std::string path = tmp.reserve("out.wav");
		std::string err;
		CHECK(writeWav(path, cap, err));
		CHECK(err.empty());

		std::vector<unsigned char> wav = readFile(path);
		CHECK(wav.size() == 44 + 500 * 4);
		if (wav.size() < 44 + 500 * 4) return;

		CHECK(tagAt(wav, 0, "RIFF"));
		CHECK(tagAt(wav, 8, "WAVE"));
		CHECK(tagAt(wav, 12, "fmt "));
		CHECK(tagAt(wav, 36, "data"));
		CHECK_EQ(le32(wav, 16), 16u);			// PCM header size
		CHECK_EQ(le16s(wav, 20), 1);			// format: PCM
		CHECK_EQ(le16s(wav, 22), 2);			// stereo
		CHECK_EQ(le32(wav, 24), 22050u);		// the rate that was asked for
		CHECK_EQ(le32(wav, 28), 22050u * 4);		// byte rate
		CHECK_EQ(le16s(wav, 32), 4);			// block align
		CHECK_EQ(le16s(wav, 34), 16);			// bits per sample
		CHECK_EQ(le32(wav, 40), (uint32_t)(500 * 4));	// data size
		// RIFF size counts everything after the first eight bytes.
		CHECK_EQ(le32(wav, 4), (uint32_t)(36 + 500 * 4));

		// The samples came out centred on zero and scaled to a sensible level:
		// a peak of about 28000 either way, and the two channels in opposite
		// phase because that is how they went in.
		int peak = 0;
		long long sum = 0;
		for (int i = 0; i < 500 * 2; i++) {
			const int v = le16s(wav, 44 + i * 2);
			if (std::abs(v) > peak) peak = std::abs(v);
			sum += v;
		}
		CHECK(peak > 27000 && peak <= 28000);
		CHECK(std::llabs(sum) < 1000);			// the DC is gone
		CHECK(le16s(wav, 44) == -le16s(wav, 46));	// left and right opposed
	}

	// A path that cannot be opened is reported rather than swallowed.
	{
		unit::TempDir tmp("audio-bad");
		Capture cap;
		cap.start(44100, true, false);
		std::vector<int> one(4, 1000);
		feedSamples(cap, one, one, 1000000LL);
		std::string err;
		CHECK(!writeWav(tmp.path() + "/no-such-dir/x.wav", cap, err));
		CHECK(!err.empty());
	}
}
