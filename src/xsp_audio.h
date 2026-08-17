// Hearing. The core mixes beeper + AY + tape into hw->vol(), and compExec()
// already advances the sound chips, so all we have to do is sample the output
// while the machine runs - and decode the AY registers into something an agent
// can reason about (this note, that volume, that envelope).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "spectrum.h"
}

namespace xsp {
namespace audio {

struct AyChannel {
	char name = 'A';
	int period = 0;
	double hz = 0.0;	// tone frequency
	int volume = 0;		// 0..15
	bool tone = false;	// tone enabled in the mixer
	bool noise = false;	// noise enabled in the mixer
	bool envelope = false;	// volume driven by the envelope generator
	bool level = false;	// current output level
};

struct AyState {
	bool present = false;
	std::string chip = "none";
	double frq_mhz = 0.0;
	unsigned char reg[16] = {0};
	AyChannel ch[3];
	int noise_period = 0;
	double noise_hz = 0.0;
	int env_period = 0;
	double env_hz = 0.0;
	int env_shape = 0;
	bool audible = false;	// at least one channel with volume and a source
};

AyState ayState(Computer* comp, int chipIndex);

// One AY register write seen while watching.
struct AyWrite {
	int frame = 0;
	int t_state = 0;
	int reg = 0;
	int value = 0;
	int pc = 0;
};

// AC energy of one sound source, so we can say where the noise comes from.
struct SourceEnergy {
	long long sum = 0;
	long long sq = 0;
	size_t n = 0;
	double rms() const;		// standard deviation = the part you can hear
};

// Rolling capture of the mixed output, filled from the execution loop.
struct Capture {
	bool on = false;
	int rate = 44100;
	bool keep_pcm = true;

	long long ns_acc = 0;		// time since the last sample
	long long ns_total = 0;
	size_t samples = 0;

	long long sum_l = 0, sum_r = 0;
	long long sq_l = 0, sq_r = 0;
	int min_l = 0, max_l = 0, min_r = 0, max_r = 0;
	int beeper_toggles = 0;
	int last_beeper_level = -1;
	int zero_crossings = 0;
	int last_sign = 0;
	double running_mean = 0.0;

	SourceEnergy beeper, ay, gs;	// per-source breakdown (side-effect-free reads)

	std::vector<int16_t> pcm;	// interleaved stereo, DC-removed and normalised

	// AY register watching shares the capture's lifetime
	bool watch = false;
	std::vector<AyWrite> writes;
	unsigned char lastReg[16] = {0};
	bool lastRegValid = false;

	void start(int rate, bool keepPcm, bool watchAy);
	void stop();
};

// Called from the execution loop after every instruction with the time it took.
void tick(Computer* comp, Capture& cap, long long ns, int frame, int pc);

struct Summary {
	int rate = 0;
	double duration_s = 0.0;
	size_t samples = 0;
	int peak = 0;			// peak deviation from the DC level
	double rms = 0.0;
	bool silent = true;
	double dominant_hz = 0.0;	// zero-crossing estimate, rough but honest
	int beeper_toggles = 0;
	int raw_min = 0, raw_max = 0;
	double beeper_rms = 0.0, ay_rms = 0.0, gs_rms = 0.0;
	std::string dominant_source = "silence";
};

Summary summarize(const Capture& cap);

// 16-bit stereo PCM WAV. Returns false and fills err on failure.
bool writeWav(const std::string& path, const Capture& cap, std::string& err);

} // namespace audio
} // namespace xsp
