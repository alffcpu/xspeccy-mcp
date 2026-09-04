// Sound: the AY registers, and what the mixer actually produced.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerAudioTools() {
	tool("ay_state",
	     "Decode the AY/YM sound chip: raw registers plus, per channel, the tone period as a "
	     "frequency in Hz, the volume, whether tone/noise are enabled in the mixer and whether the "
	     "envelope drives the volume. `audible` says whether any channel could currently be heard.",
	     json{{"properties", {{"chip", {{"type", "integer"}, {"description", "0=A (default), 1=B for Turbo Sound"}}}}}},
	     [](const json& a) {
		     auto st = xsp::audio::ayState(mach().comp(), argNumOr(a, "chip", 0));
		     if (!st.present) return json{{"present", false}};
		     json regs = json::array();
		     std::string hex;
		     char buf[8];
		     for (int i = 0; i < 16; i++) {
			     regs.push_back(st.reg[i]);
			     snprintf(buf, sizeof(buf), "%02X", st.reg[i]);
			     if (i) hex += ' ';
			     hex += buf;
		     }
		     json chans = json::array();
		     for (int i = 0; i < 3; i++) {
			     const auto& c = st.ch[i];
			     chans.push_back({
				     {"channel", std::string(1, c.name)},
				     {"period", c.period}, {"hz", c.hz},
				     {"volume", c.volume}, {"tone", c.tone},
				     {"noise", c.noise}, {"envelope", c.envelope},
				     {"level", c.level}
			     });
		     }
		     return json{
			     {"present", true}, {"chip", st.chip}, {"frq_mhz", st.frq_mhz},
			     {"registers", regs}, {"registers_hex", hex},
			     {"channels", chans},
			     {"noise", {{"period", st.noise_period}, {"hz", st.noise_hz}}},
			     {"envelope", {{"period", st.env_period}, {"hz", st.env_hz}, {"shape", st.env_shape}}},
			     {"audible", st.audible}
		     };
	     });

	tool("sound_state",
	     "Every sound source at once: beeper level, both AY chips of a Turbo Sound pair, General "
	     "Sound, SAA1099, Soundrive, and the machine's current mixed output level. Use it to see "
	     "what is even wired up before chasing silence.",
	     json{},
	     [](const json&) {
		     Computer* c = mach().comp();
		     json chips = json::array();
		     for (int i = 0; i < 2; i++) {
			     auto st = xsp::audio::ayState(c, i);
			     if (!st.present) continue;
			     chips.push_back({{"index", i}, {"chip", st.chip},
					      {"frq_mhz", st.frq_mhz}, {"audible", st.audible}});
		     }
		     sndVolume vol;
		     memset(&vol, 0, sizeof(vol));
		     vol.master = vol.beep = vol.tape = vol.ay = vol.gs = vol.sdrv = vol.saa = 100;
		     sndPair mix = {0, 0};
		     if (c->hw && c->hw->vol) mix = c->hw->vol(c, &vol);
		     sndPair ay = c->ts ? tsGetVolume(c->ts) : sndPair{0, 0};
		     sndPair gs = c->gs ? gsVolume(c->gs) : sndPair{0, 0};
		     return json{
			     {"beeper", {{"level", c->beep ? c->beep->lev : 0},
					 {"value", c->beep ? c->beep->val : 0}}},
			     {"ay_chips", chips},
			     {"ay_level", {{"left", ay.left}, {"right", ay.right}}},
			     {"general_sound", {{"level_left", gs.left}, {"level_right", gs.right}}},
			     {"saa1099", {{"enabled", c->saa && c->saa->enabled ? true : false}}},
			     {"tape", {{"playing", c->tape && c->tape->on ? true : false},
				       {"level", c->tape ? c->tape->volPlay : 0}}},
			     {"mixed_output", {{"left", mix.left}, {"right", mix.right}}}
		     };
	     });

	tool("audio_capture",
	     "Listen: run N frames while sampling the machine's mixed audio output (beeper + AY + tape). "
	     "Reports whether there is any sound at all, its peak and RMS level, a rough dominant "
	     "frequency from zero crossings, and how often the beeper toggled. With `path` it also "
	     "writes a .wav you can play. With `watch_ay` it logs every AY register write that "
	     "happened, with the frame, T-state and the PC that did it - that is your view of a music "
	     "driver at work.",
	     json{{"properties", {
		     {"frames", {{"type", "integer"}, {"description", "frames to listen for, default 50 (1 second)"}}},
		     {"rate", {{"type", "integer"}, {"description", "sample rate, default 44100"}}},
		     {"path", {{"type", "string"}, {"description", "write a .wav here"}}},
		     {"watch_ay", {{"type", "boolean"}, {"description", "log AY register writes, default false"}}},
		     {"max_writes", {{"type", "integer"}, {"description", "AY writes to return, default 32"}}}
	     }}},
	     [](const json& a) {
		     const int frames = argNumRange(a, "frames", 50, 1, 3000);
		     std::string path = argStr(a, "path");
		     bool watchAy = argBool(a, "watch_ay", false);

		     auto& cap = mach().audio();
		     cap.start(argNumOr(a, "rate", 44100), !path.empty(), watchAy);
		     json run = runToJson(mach().runFrames(frames));
		     cap.stop();

		     auto s = xsp::audio::summarize(cap);
		     json res{
			     {"frames", frames},
			     {"duration_s", s.duration_s},
			     {"samples", (long long)s.samples},
			     {"rate", s.rate},
			     {"silent", s.silent},
			     {"peak", s.peak},
			     {"rms", s.rms},
			     {"dominant_hz", s.dominant_hz},
			     {"beeper_toggles", s.beeper_toggles},
			     {"dominant_source", s.dominant_source},
			     {"source_rms", {{"beeper", s.beeper_rms}, {"ay", s.ay_rms},
					     {"general_sound", s.gs_rms}}},
			     {"raw_range", {{"min", s.raw_min}, {"max", s.raw_max}}},
			     {"run", run}
		     };
		     if (s.truncated)
			     res["note"] = "the sample buffer filled before the run ended, so the "
					   "sound stops short of the last frame. Record fewer frames, "
					   "or a lower rate, to get all of it.";
		     if (!path.empty()) {
			     std::string err;
			     if (!xsp::audio::writeWav(path, cap, err)) throw std::runtime_error(err);
			     res["wav"] = path;
		     }
		     if (watchAy) {
			     int limit = argNumRange(a, "max_writes", 32, 1, 100000);
			     json writes = json::array();
			     int n = 0;
			     for (const auto& w : cap.writes) {
				     if (n++ >= limit) break;
				     writes.push_back({{"frame", w.frame}, {"t_state", w.t_state},
						       {"register", w.reg}, {"value", w.value},
						       {"pc", w.pc}, {"pc_hex", hex16(w.pc)}});
			     }
			     res["ay_writes_total"] = (int)cap.writes.size();
			     res["ay_writes"] = writes;
		     }
		     return res;
	     });

	tool("ay_writes",
	     "The AY register writes recorded by the last audio_capture that had watch_ay on, from "
	     "`offset`. Use it to page through a music driver's whole frame.",
	     json{{"properties", {
		     {"offset", {{"type", "integer"}}},
		     {"limit", {{"type", "integer"}, {"description", "default 64"}}}
	     }}},
	     [](const json& a) {
		     const auto& cap = mach().audio();
		     int offset = argNumRange(a, "offset", 0, 0, 0x7fffffff);
		     int limit = argNumRange(a, "limit", 64, 1, 100000);
		     json writes = json::array();
		     // end computed wide: offset near INT_MAX plus a limit overflows
		     const long long end = (long long)offset + limit;
		     for (int i = offset; i < (int)cap.writes.size() && i < end; i++) {
			     const auto& w = cap.writes[i];
			     writes.push_back({{"frame", w.frame}, {"t_state", w.t_state},
					       {"register", w.reg}, {"value", w.value},
					       {"pc", w.pc}, {"pc_hex", hex16(w.pc)}});
		     }
		     return json{{"total", (int)cap.writes.size()}, {"offset", offset}, {"writes", writes}};
	     });

	// -------------------------------------------------- keyboard
}

} // namespace tools
} // namespace xsp
