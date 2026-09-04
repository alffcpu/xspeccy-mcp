// Running the machine: whole frames, single steps, and stopping.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerExecutionTools() {
	tool("run",
	     "Run the CPU until something stops it: a breakpoint, stop_pc, max_frames or the "
	     "max_instructions budget. Video and interrupts stay live, so this is safe for code that "
	     "waits for the raster. Returns the stop reason.",
	     json{{"properties", {
		     {"max_instructions", {{"type", "integer"}, {"description", "budget, default 10000000"}}},
		     {"stop_pc", {{"type", "string"}, {"description", "stop when PC reaches this address; label names work"}}},
		     {"max_frames", {{"type", "integer"}, {"description", "stop after this many frames"}}}
	     }}},
	     [](const json& a) {
		     // A budget below 1 disables every stop condition in execLoop and
		     // the call never returns. The server reads the next message only
		     // after the current one is answered, so that is not a slow tool
		     // call - it is a server that stops responding.
		     long long budget = argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff);
		     int stopPc = -1;
		     argAddr(a, "stop_pc", stopPc);
		     int frames = argNumRange(a, "max_frames", -1, 1, kMaxFrames);
		     return runToJson(mach().run(budget, stopPc, frames));
	     });

	tool("run_frames",
	     "Run exactly N timing-accurate frames (interrupts + video). Use this to let an effect "
	     "settle before a screenshot: one frame is 1/50 s of emulated time.",
	     json{{"properties", {{"count", {{"type", "integer"},
					     {"minimum", 1}, {"maximum", kMaxFrames},
					     {"description", "frames, default 1"}}}}}},
	     [](const json& a) {
		     return runToJson(mach().runFrames(argNumRange(a, "count", 1, 1, kMaxFrames)));
	     });

	tool("run_to_beam",
	     "Run until the video beam reaches a raster position, which is how raster code is "
	     "stepped: by where the picture is being drawn rather than by address or instruction "
	     "count. Give a line, and optionally a dot within it. A target already behind the beam "
	     "means the next frame, not an immediate stop - so calling this repeatedly with the same "
	     "line walks the same point of the raster frame after frame. Stops on the first "
	     "instruction that reached or passed the target and reports where it actually landed, "
	     "since the beam only moves between instructions and lands a few T-states past it.",
	     json{{"properties", {
		     {"line", {{"type", "integer"}, {"description", "raster line in the full frame (see beam_position)"}}},
		     {"dot", {{"type", "integer"}, {"description", "dot within the line; omitted means anywhere on it"}}},
		     {"max_instructions", {{"type", "integer"}, {"description", "budget, default 10000000"}}}
	     }}, {"required", json::array({"line"})}},
	     [](const json& a) {
		     Video* v = mach().comp()->vid;
		     const int line = argNumRange(a, "line", -1, 0, v->full.y > 0 ? v->full.y - 1 : 0xffff);
		     if (line < 0) throw std::runtime_error("line required");
		     const int dot = argNumRange(a, "dot", -1, 0, v->full.x > 0 ? v->full.x - 1 : 0xffff);
		     long long budget = argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff);
		     json j = runToJson(mach().runToBeam(line, dot, budget));
		     j["target"] = {{"line", line}, {"dot", dot}};
		     if (j.contains("beam")) {
			     // how far past the target we actually stopped, in dots and lines
			     const int gotLine = j["beam"]["line"].get<int>();
			     const int gotDot = j["beam"]["dot"].get<int>();
			     j["overshoot_lines"] = gotLine - line;
			     if (dot >= 0 && gotLine == line) j["overshoot_dots"] = gotDot - dot;
		     }
		     return j;
	     });

	tool("step",
	     "Execute N instructions, ignoring breakpoints. Use step_over instead when the next "
	     "instruction is a CALL you do not want to walk through, and step_out to finish the "
	     "routine you are in.",
	     json{{"properties", {{"count", {{"type", "integer"}, {"minimum", 1}}}}}},
	     [](const json& a) { return runToJson(mach().step(argNumRange(a, "count", 1, 1, 0x7fffffff))); });

	tool("step_over",
	     "Execute one instruction, but run CALL/RST/block instructions to completion. Gives up "
	     "after max_instructions if the call never comes back.",
	     json{{"properties", {{"max_instructions", {{"type", "integer"},
						       {"description", "budget, default 10000000"}}}}}},
	     [](const json& a) {
		     return runToJson(mach().stepOver(argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff)));
	     });

	tool("step_out",
	     "Run until the current subroutine returns (SP rises above its current value). Gives up "
	     "after max_instructions if it never does.",
	     json{{"properties", {{"max_instructions", {{"type", "integer"},
						       {"description", "budget, default 10000000"}}}}}},
	     [](const json& a) {
		     return runToJson(mach().stepOut(argNumRange(a, "max_instructions", 10000000, 1, 0x7fffffff)));
	     });

	// -------------------------------------------------- cpu / memory
}

} // namespace tools
} // namespace xsp
