// What machine this is, and changing it.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerMachineTools() {
	tool("machine_config",
	     "Read or change the machine. Give a model name (see list_models) and everything else "
	     "- romset, RAM size, screen geometry - comes from the emulator's own defaults for it. "
	     "No arguments = report the current configuration. Changing the model resets the machine.",
	     json{{"properties", {
		     {"model", {{"type", "string"}, {"description", "machine name, e.g. Pentagon, ZX48K, Scorpion"}}},
		     {"memory", {{"type", "integer"}, {"description", "RAM in KB; clamped to what the model supports"}}},
		     {"romset", {{"type", "string"}, {"description", "romset name from the emulator config"}}},
		     {"geometry", {{"type", "string"}, {"description", "screen layout name, e.g. default, Pentagon"}}},
		     {"boot_frames", {{"type", "integer"}, {"description", "frames to run after the reset (default 100)"}}}
	     }}},
	     [](const json& a) {
		     json changed = json::array();
		     std::string err;
		     bool touched = false;
		     if (a.contains("model")) {
			     if (!mach().setModel(argStr(a, "model"), err)) throw std::runtime_error(err);
			     changed.push_back("model"); touched = true;
		     }
		     if (a.contains("memory")) {
			     if (!mach().setMemory(argNumOr(a, "memory", 128), err)) throw std::runtime_error(err);
			     changed.push_back("memory"); touched = true;
		     }
		     if (a.contains("romset")) {
			     if (!mach().setRomset(argStr(a, "romset"), err)) throw std::runtime_error(err);
			     changed.push_back("romset"); touched = true;
		     }
		     if (a.contains("geometry")) {
			     if (!mach().setLayout(argStr(a, "geometry"), err)) throw std::runtime_error(err);
			     changed.push_back("geometry"); touched = true;
		     }
		     json res = machineStateJson();
		     res["changed"] = changed;
		     res["config_source"] = mach().env().note;
		     if (touched) {
			     mach().reset(RES_DEFAULT);
			     int boot = argNumOr(a, "boot_frames", 100);
			     if (boot > 0) res["boot"] = runToJson(mach().runFrames(boot));
			     res["pc"] = cpu_get_pc(mach().comp()->cpu);
			     res["pc_hex"] = hex16(cpu_get_pc(mach().comp()->cpu));
		     }
		     return res;
	     });

	tool("list_models",
	     "Every machine name this build of the emulator core knows, plus the romsets and screen "
	     "geometries defined in the emulator's configuration. A handful of the machines are "
	     "emulated by the core but cannot be run headlessly - machine_config refuses those and "
	     "says why - so this is the list to choose from rather than a guarantee.",
	     json{},
	     [](const json&) {
		     json romsets = json::array();
		     for (const auto& r : mach().env().romsets) romsets.push_back(r.name);
		     json layouts = json::array();
		     for (const auto& l : mach().env().layouts) layouts.push_back(l.name);
		     return json{{"models", mach().models()}, {"romsets", romsets}, {"geometries", layouts}};
	     });

	tool("machine_state",
	     "Current machine: model, RAM, romset, CPU, frame timing, screen size and the 16K page map "
	     "(which ROM/RAM bank is visible at $0000/$4000/$8000/$C000).",
	     json{},
	     [](const json&) { return machineStateJson(); });

	tool("video_config",
	     "Read or change how frames are turned into pictures. No arguments = report.\n"
	     "blend is the one that matters for flickering effects: set it here and every later "
	     "screenshot, frame_digest and record_video averages that many frames without being told "
	     "again. 2 is a gigascreen, 3 a three-frame effect, 1 the raw frame. The averaging happens "
	     "in linear light, which is why a mix of black and white comes out as the mid grey the eye "
	     "sees rather than the darker one averaging the bytes would give; gamma is that transfer, "
	     "and gamma 1 turns it off. frame_history is how many completed frames are kept for "
	     "blending to draw on - blending more frames than are kept is not possible.\n"
	     "greyscale and border_size are emulator settings this server otherwise takes defaults "
	     "for. greyscale applies as frames are drawn, so it shows up on the next frame run, not "
	     "on the last one already in the buffer.",
	     json{{"properties", {
		     {"blend", {{"type", "integer"}, {"description",
				"default frames to average, 1..8; 1 = off"}}},
		     {"gamma", {{"description", "transfer used for the blend, 1.0..3.0, default 2.2; "
				 "1.0 averages the sRGB bytes directly"}}},
		     {"frame_history", {{"type", "integer"}, {"description",
					"completed frames to keep, 0..8, default 4. 0 stops recording "
					"them, which makes blending impossible and saves one frame-sized "
					"copy per emulated frame"}}},
		     {"greyscale", {{"type", "boolean"}, {"description", "draw through the grey palette"}}},
		     {"border_size", {{"type", "integer"}, {"description",
				      "percentage of the border the frame carries, 0..100"}}}
	     }}},
	     [](const json& a) {
		     json changed = json::array();
		     auto& def = xsp::blend::defaults();
		     if (a.is_object() && a.contains("blend") && !a["blend"].is_null()) {
			     def.frames = argNumRange(a, "blend", def.frames, 1, xsp::blend::kMaxHistory);
			     changed.push_back("blend");
		     }
		     double g = def.gamma;
		     if (argDouble(a, "gamma", g)) {
			     if (g < 1.0 || g > 3.0)
				     throw std::runtime_error("bad gamma: " + std::to_string(g) +
							      " is outside 1.0..3.0");
			     def.gamma = g;
			     changed.push_back("gamma");
		     }
		     if (a.is_object() && a.contains("frame_history") && !a["frame_history"].is_null()) {
			     def.history = argNumRange(a, "frame_history", def.history, 0,
						       xsp::blend::kMaxHistory);
			     xsp::blend::setDepth(def.history);
			     changed.push_back("frame_history");
		     }
		     if (a.is_object() && a.contains("greyscale") && !a["greyscale"].is_null()) {
			     greyScale = argBool(a, "greyscale", false) ? 1 : 0;
			     changed.push_back("greyscale");
		     }
		     if (a.is_object() && a.contains("border_size") && !a["border_size"].is_null()) {
			     const int pc = argNumRange(a, "border_size", 50, 0, 100);
			     mach().setBorderSize(pc / 100.0);
			     changed.push_back("border_size");
		     }

		     Computer* c = mach().comp();
		     json pal = json::array();
		     for (int i = 0; i < 16; i++) {
			     char buf[16];
			     const uint32_t v = c->vid->pal[i];
			     snprintf(buf, sizeof(buf), "#%02X%02X%02X",
				      (unsigned)(v & 0xff), (unsigned)((v >> 8) & 0xff),
				      (unsigned)((v >> 16) & 0xff));
			     pal.push_back(buf);
		     }
		     json res{
			     {"blend", def.frames},
			     {"gamma", def.gamma},
			     {"frame_history", xsp::blend::depth()},
			     {"frames_recorded", xsp::blend::filled()},
			     {"greyscale", greyScale != 0},
			     {"border_size", (int)(mach().borderSize() * 100 + 0.5)},
			     {"screen", {{"width", c->vid->vsze.x}, {"height", c->vid->vsze.y}}},
			     {"palette", pal}
		     };
		     if (!changed.empty()) res["changed"] = changed;
		     if (def.frames > xsp::blend::depth())
			     res["warning"] = "blend asks for more frames than frame_history keeps";
		     return res;
	     });

	tool("settings",
	     "Every emulator setting this server can change, what it is set to now, and where that "
	     "value came from. Without arguments it reports all of them, which is the way to answer "
	     "\"what machine am I actually running\" - several of these were fixed in the source "
	     "before and could not be seen at all, so a server could quietly differ from the "
	     "emulator's own configuration. action: report (default), set, reset.\n"
	     "\n"
	     "`set` changes one for the session; `save: true` also writes it into a settings file, "
	     "so a project keeps its machine in its own repository and every session starts the "
	     "same. `scope` on each row says whether a change takes effect at once, needs a reset, "
	     "or is read only at startup. Changing anything under `timing.` invalidates every "
	     "frame_cost, profile and beam_log measured before it, and the reply says so.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "report (default) | set | reset"}}},
		     {"name", {{"type", "string"}, {"description", "setting name, e.g. timing.contPattern"}}},
		     {"value", {{"description", "new value; enums by name, booleans as yes/no"}}},
		     {"save", {{"type", "boolean"}, {"description",
			       "with set, also write it to the settings file"}}},
		     {"path", {{"type", "string"}, {"description",
			       "settings file to write; default is beside the Xpeccy config"}}}
	     }}},
	     [](const json& a) {
		     namespace st = xsp::settings;
		     const std::string action = argStr(a, "action", "report");

		     auto describe = [](const st::Setting& s) {
			     json row{
				     {"name", s.name},
				     {"value", st::formatValue(s, s.get(mach()))},
				     {"default", st::formatValue(s, s.def)},
				     {"source", st::sourceName(st::sourceOf(s.name))},
				     {"scope", s.scope == st::Scope::Live ? "live"
					       : s.scope == st::Scope::NeedsReset ? "needs reset"
										  : "needs restart"},
				     {"affects", s.affects}
			     };
			     if (s.xpeccyKey) row["xpeccy_key"] = s.xpeccyKey;
			     if (s.type == st::Type::Enum) {
				     json opts = json::array();
				     for (int i = 0; s.enumNames[i]; i++) opts.push_back(s.enumNames[i]);
				     row["options"] = opts;
			     } else if (s.type != st::Type::Bool) {
				     row["min"] = st::formatValue(s, s.lo);
				     row["max"] = st::formatValue(s, s.hi);
			     }
			     if (s.flags & st::TimingBaseline)
				     row["invalidates"] = "timing baselines measured before the change";
			     if (s.flags & st::DropsHistory)
				     row["invalidates"] = "the blended-frame history";
			     return row;
		     };

		     if (action == "report") {
			     json rows = json::array();
			     for (const auto& s : st::table()) rows.push_back(describe(s));
			     return json{
				     {"settings", rows},
				     {"count", (int)st::table().size()},
				     {"settings_file", st::defaultSavePath(mach().env().confDir)},
				     {"xpeccy_config", mach().env().confDir},
				     {"note", "settings fixed when the server was built are not "
					      "listed here, because listing them would suggest "
					      "they could be changed"}
			     };
		     }

		     const std::string name = argStr(a, "name", "");
		     if (name.empty()) throw std::runtime_error("name required");
		     const st::Setting* s = st::find(name);
		     if (!s) throw std::runtime_error("no such setting: " + name +
						      " (call settings with no arguments for the list)");

		     if (action == "reset") {
			     std::string err;
			     if (!st::apply(mach(), *s, s->def, err)) throw std::runtime_error(err);
			     st::markSource(s->name, st::Source::Default);
			     return json{{"changed", describe(*s)}};
		     }
		     if (action != "set")
			     throw std::runtime_error("action must be report, set or reset");

		     if (!a.contains("value") || a["value"].is_null())
			     throw std::runtime_error("value required");
		     std::string text;
		     if (a["value"].is_string()) text = a["value"].get<std::string>();
		     else if (a["value"].is_boolean()) text = a["value"].get<bool>() ? "yes" : "no";
		     else text = a["value"].dump();

		     double v = 0;
		     std::string err;
		     if (!st::parseValue(*s, text, v, err)) throw std::runtime_error(err);
		     if (!st::apply(mach(), *s, v, err)) throw std::runtime_error(err);
		     st::markSource(s->name, st::Source::Session);

		     json res{{"changed", describe(*s)}};
		     if (s->scope == st::Scope::NeedsReset)
			     res["note"] = "stored, but the machine has to be reset before it means "
					   "anything";
		     if (s->flags & st::TimingBaseline)
			     res["note"] = "every timing measurement taken before this is no longer "
					   "comparable; measure again";
		     if (argBool(a, "save", false)) {
			     std::string path = argStr(a, "path", "");
			     if (path.empty()) path = st::defaultSavePath(mach().env().confDir);
			     std::string used, serr;
			     if (!st::save(path, s->name, st::formatValue(*s, v), used, serr))
				     throw std::runtime_error(serr);
			     res["saved_to"] = used;
		     }
		     return res;
	     });

	tool("reset",
	     "Reset the machine. mode: default, 48, 128, dos, shadow. Runs boot_frames afterwards "
	     "(default 100) so the machine reaches its prompt.",
	     json{{"properties", {
		     {"mode", {{"type", "string"}}},
		     {"boot_frames", {{"type", "integer"}}}
	     }}},
	     [](const json& a) {
		     std::string m = argStr(a, "mode", "default");
		     int mode = RES_DEFAULT;
		     if (m == "48") mode = RES_48;
		     else if (m == "128") mode = RES_128;
		     else if (m == "dos") mode = RES_DOS;
		     else if (m == "shadow") mode = RES_SHADOW;
		     mach().reset(mode);
		     int boot = argNumOr(a, "boot_frames", 100);
		     json res{{"mode", m}};
		     if (boot > 0) res["boot"] = runToJson(mach().runFrames(boot));
		     res["pc"] = cpu_get_pc(mach().comp()->cpu);
		     res["pc_hex"] = hex16(cpu_get_pc(mach().comp()->cpu));
		     return res;
	     });

	// -------------------------------------------------- execution
}

} // namespace tools
} // namespace xsp
