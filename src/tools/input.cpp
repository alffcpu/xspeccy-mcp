// The keyboard, as a person would use it.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerInputTools() {
	tool("press_key",
	     "Hold a key for a few frames and release it. Names: a-z, 0-9, enter, space, caps, symbol, "
	     "delete, break, left/right/up/down, edit, capslock, graph, extend. The ROM needs the key "
	     "held for at least two frames to notice it.",
	     json{{"properties", {
		     {"key", {{"type", "string"}}},
		     {"frames", {{"type", "integer"}, {"description", "frames to hold, default 3"}}},
		     {"release_frames", {{"type", "integer"}, {"description", "frames after release, default 3"}}}
	     }}, {"required", json::array({"key"})}},
	     [](const json& a) {
		     xsp::keyboard::Key key;
		     std::string name = argStr(a, "key");
		     if (!xsp::keyboard::lookup(name, key))
			     throw std::runtime_error("unknown key '" + name + "'");
		     int hold = argNumRange(a, "frames", 3, 1, kMaxFrames);
		     int gap = argNumRange(a, "release_frames", 3, 0, kMaxFrames);
		     xsp::keyboard::press(mach().comp(), key);
		     mach().runFrames(hold);
		     xsp::keyboard::release(mach().comp(), key);
		     if (gap > 0) mach().runFrames(gap);
		     return json{{"key", name}, {"frames", hold + gap},
			     {"pc", cpu_get_pc(mach().comp()->cpu)},
			     {"pc_hex", hex16(cpu_get_pc(mach().comp()->cpu))}};
	     });

	tool("type_text",
	     "Type a string on the ZX keyboard, key by key, letting the machine run between presses. "
	     "Upper case and symbols get their shift automatically; \\n presses Enter. Remember the "
	     "48K ROM is in keyword-entry mode, so \"LOAD\" is one keypress (j), not four.",
	     json{{"properties", {
		     {"text", {{"type", "string"}}},
		     {"frames", {{"type", "integer"}, {"description", "frames per key, default 3"}}}
	     }}, {"required", json::array({"text"})}},
	     [](const json& a) {
		     std::string text = argStr(a, "text");
		     int hold = argNumRange(a, "frames", 3, 1, kMaxFrames);
		     int typed = 0;
		     std::string skipped;
		     for (char ch : text) {
			     xsp::keyboard::Key key;
			     if (!xsp::keyboard::forChar(ch, key)) { skipped += ch; continue; }
			     xsp::keyboard::press(mach().comp(), key);
			     mach().runFrames(hold);
			     xsp::keyboard::release(mach().comp(), key);
			     mach().runFrames(hold);
			     typed++;
		     }
		     return json{{"typed", typed}, {"skipped", skipped},
			     {"pc", cpu_get_pc(mach().comp()->cpu)},
			     {"pc_hex", hex16(cpu_get_pc(mach().comp()->cpu))}};
	     });

	tool("release_keys",
	     "Release every key - use it if a held key got stuck.",
	     json{},
	     [](const json&) {
		     xsp::keyboard::releaseAll(mach().comp());
		     return json{{"ok", true}};
	     });

	// -------------------------------------------------- files
}

} // namespace tools
} // namespace xsp
