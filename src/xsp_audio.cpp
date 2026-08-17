#include "xsp_audio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "hardware/hardware.h"
}

namespace xsp {
namespace audio {

static aymChip* chipAt(Computer* comp, int index) {
	if (!comp || !comp->ts) return nullptr;
	switch (index) {
		case 0: return comp->ts->chipA;
		case 1: return comp->ts->chipB;
		case 2: return comp->ts->chipC;
		case 3: return comp->ts->chipD;
	}
	return nullptr;
}

AyState ayState(Computer* comp, int index) {
	AyState st;
	aymChip* chip = chipAt(comp, index);
	if (!chip || chip->type == SND_NONE) return st;

	st.present = true;
	st.frq_mhz = chip->frq;
	switch (chip->type) {
		case SND_AY: st.chip = "AY-3-8910"; break;
		case SND_YM: st.chip = "YM2149"; break;
		case SND_YM2203: st.chip = "YM2203"; break;
		default: st.chip = "unknown"; break;
	}
	for (int i = 0; i < 16; i++) st.reg[i] = chip->reg[i];

	const double hz = chip->frq * 1e6;
	const aymChan* chans[3] = {&chip->chanA, &chip->chanB, &chip->chanC};
	const int mixer = st.reg[7];
	for (int i = 0; i < 3; i++) {
		AyChannel& c = st.ch[i];
		c.name = (char)('A' + i);
		c.period = ((st.reg[i * 2 + 1] & 0x0f) << 8) | st.reg[i * 2];
		c.hz = c.period > 0 ? hz / (16.0 * c.period) : 0.0;
		c.volume = st.reg[8 + i] & 0x0f;
		c.envelope = (st.reg[8 + i] & 0x10) != 0;
		c.tone = !(mixer & (1 << i));		// 0 in the mixer means enabled
		c.noise = !(mixer & (1 << (i + 3)));
		c.level = chans[i]->lev ? true : false;
		if ((c.volume > 0 || c.envelope) && (c.tone || c.noise)) st.audible = true;
	}

	st.noise_period = st.reg[6] & 0x1f;
	st.noise_hz = st.noise_period > 0 ? hz / (16.0 * st.noise_period) : 0.0;
	st.env_period = (st.reg[12] << 8) | st.reg[11];
	st.env_hz = st.env_period > 0 ? hz / (256.0 * st.env_period) : 0.0;
	st.env_shape = st.reg[13] & 0x0f;
	return st;
}

double SourceEnergy::rms() const {
	if (!n) return 0.0;
	const double mean = (double)sum / n;
	const double var = (double)sq / n - mean * mean;
	return var > 0.0 ? std::sqrt(var) : 0.0;
}

static void feed(SourceEnergy& e, int v) {
	e.sum += v;
	e.sq += (long long)v * v;
	e.n++;
}

void Capture::start(int r, bool keepPcm, bool watchAy) {
	if (r < 4000) r = 4000;
	if (r > 192000) r = 192000;
	rate = r;
	keep_pcm = keepPcm;
	watch = watchAy;
	ns_acc = ns_total = 0;
	samples = 0;
	sum_l = sum_r = sq_l = sq_r = 0;
	min_l = min_r = 0x7fffffff;
	max_l = max_r = -0x7fffffff;
	beeper_toggles = 0;
	last_beeper_level = -1;
	zero_crossings = 0;
	last_sign = 0;
	running_mean = 0.0;
	beeper = SourceEnergy();
	ay = SourceEnergy();
	gs = SourceEnergy();
	pcm.clear();
	writes.clear();
	lastRegValid = false;
	on = true;
}

void Capture::stop() {
	on = false;
}

void tick(Computer* comp, Capture& cap, long long ns, int frame, int pc) {
	if (!cap.on || !comp) return;

	// beeper edges: the cheapest "is anything making noise" signal there is
	if (comp->beep) {
		int lev = comp->beep->lev ? 1 : 0;
		if (cap.last_beeper_level >= 0 && lev != cap.last_beeper_level) cap.beeper_toggles++;
		cap.last_beeper_level = lev;
	}

	// AY register writes, spotted by diffing (the core has no write hook)
	if (cap.watch && comp->ts && comp->ts->chipA) {
		const unsigned char* reg = comp->ts->chipA->reg;
		if (!cap.lastRegValid) {
			memcpy(cap.lastReg, reg, 16);
			cap.lastRegValid = true;
		} else {
			for (int i = 0; i < 16; i++) {
				if (reg[i] != cap.lastReg[i]) {
					if (cap.writes.size() < 8192)
						cap.writes.push_back(AyWrite{frame, comp->frmtCount, i, reg[i], pc});
					cap.lastReg[i] = reg[i];
				}
			}
		}
	}

	cap.ns_acc += ns;
	cap.ns_total += ns;
	const long long nsPerSample = 1000000000LL / cap.rate;
	while (cap.ns_acc >= nsPerSample) {
		cap.ns_acc -= nsPerSample;

		sndVolume vol;
		memset(&vol, 0, sizeof(vol));
		vol.master = 100; vol.beep = 100; vol.tape = 100;
		vol.ay = 100; vol.gs = 100; vol.sdrv = 100; vol.saa = 100;

		sndPair p = {0, 0};
		if (comp->hw && comp->hw->vol) p = comp->hw->vol(comp, &vol);
		const int l = (int)p.left;
		const int r = (int)p.right;

		// where is the sound coming from? (all three reads are pure)
		if (comp->beep) feed(cap.beeper, comp->beep->val);
		if (comp->ts) {
			sndPair a = tsGetVolume(comp->ts);
			feed(cap.ay, (int)((a.left + a.right) / 2));
		}
		if (comp->gs) {
			sndPair g = gsVolume(comp->gs);
			feed(cap.gs, (int)((g.left + g.right) / 2));
		}

		cap.sum_l += l;   cap.sum_r += r;
		cap.sq_l += (long long)l * l;
		cap.sq_r += (long long)r * r;
		if (l < cap.min_l) cap.min_l = l;
		if (l > cap.max_l) cap.max_l = l;
		if (r < cap.min_r) cap.min_r = r;
		if (r > cap.max_r) cap.max_r = r;

		// zero crossings against a slow-moving mean = rough pitch estimate
		const double mono = (l + r) / 2.0;
		cap.running_mean += (mono - cap.running_mean) * 0.001;
		const int sign = mono > cap.running_mean ? 1 : -1;
		if (cap.last_sign && sign != cap.last_sign) cap.zero_crossings++;
		cap.last_sign = sign;

		if (cap.keep_pcm && cap.pcm.size() < 2 * 44100 * 60) {	// cap at a minute
			cap.pcm.push_back((int16_t)l);
			cap.pcm.push_back((int16_t)r);
		}
		cap.samples++;
	}
}

Summary summarize(const Capture& cap) {
	Summary s;
	s.rate = cap.rate;
	s.samples = cap.samples;
	s.duration_s = cap.ns_total / 1e9;
	s.beeper_toggles = cap.beeper_toggles;
	if (!cap.samples) return s;

	const double meanL = (double)cap.sum_l / cap.samples;
	const double meanR = (double)cap.sum_r / cap.samples;
	const double varL = (double)cap.sq_l / cap.samples - meanL * meanL;
	const double varR = (double)cap.sq_r / cap.samples - meanR * meanR;
	s.rms = std::sqrt(std::max(0.0, (varL + varR) / 2.0));	// AC part only

	const int peakL = (int)std::max(std::fabs(cap.max_l - meanL), std::fabs(meanL - cap.min_l));
	const int peakR = (int)std::max(std::fabs(cap.max_r - meanR), std::fabs(meanR - cap.min_r));
	s.peak = std::max(peakL, peakR);
	s.raw_min = std::min(cap.min_l, cap.min_r);
	s.raw_max = std::max(cap.max_l, cap.max_r);

	s.beeper_rms = cap.beeper.rms();
	s.ay_rms = cap.ay.rms();
	s.gs_rms = cap.gs.rms();
	if (s.beeper_rms >= s.ay_rms && s.beeper_rms >= s.gs_rms && s.beeper_rms > 1.0)
		s.dominant_source = "beeper";
	else if (s.ay_rms >= s.gs_rms && s.ay_rms > 1.0)
		s.dominant_source = "ay";
	else if (s.gs_rms > 1.0)
		s.dominant_source = "general_sound";

	s.silent = s.peak < 8 && cap.beeper_toggles == 0;
	if (s.duration_s > 0.0)
		s.dominant_hz = cap.zero_crossings / 2.0 / s.duration_s;
	return s;
}

static void put32(FILE* f, uint32_t v) {
	unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8),
			      (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
	fwrite(b, 1, 4, f);
}

static void put16(FILE* f, uint16_t v) {
	unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
	fwrite(b, 1, 2, f);
}

bool writeWav(const std::string& path, const Capture& cap, std::string& err) {
	if (cap.pcm.empty()) { err = "nothing captured (keep_pcm was off?)"; return false; }

	// the core's mixer output is unsigned and not centred; make it a normal
	// signed waveform so the file is actually listenable
	double mean = 0.0;
	for (int16_t v : cap.pcm) mean += (uint16_t)v;
	mean /= cap.pcm.size();
	double peak = 1.0;
	for (int16_t v : cap.pcm) peak = std::max(peak, std::fabs((uint16_t)v - mean));
	const double gain = 28000.0 / peak;

	FILE* f = fopen(path.c_str(), "wb");
	if (!f) { err = "can't write '" + path + "'"; return false; }

	const uint32_t dataBytes = (uint32_t)(cap.pcm.size() * 2);
	fwrite("RIFF", 1, 4, f);
	put32(f, 36 + dataBytes);
	fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16);			// PCM header size
	put16(f, 1);			// format: PCM
	put16(f, 2);			// stereo
	put32(f, (uint32_t)cap.rate);
	put32(f, (uint32_t)cap.rate * 4);	// byte rate
	put16(f, 4);			// block align
	put16(f, 16);			// bits per sample
	fwrite("data", 1, 4, f);
	put32(f, dataBytes);
	for (int16_t v : cap.pcm) {
		double s = ((uint16_t)v - mean) * gain;
		if (s > 32767.0) s = 32767.0;
		if (s < -32768.0) s = -32768.0;
		put16(f, (uint16_t)(int16_t)s);
	}
	bool ok = !ferror(f);
	fclose(f);
	if (!ok) err = "write error on '" + path + "'";
	return ok;
}

} // namespace audio
} // namespace xsp
