#include "xsp_server.h"

#include <cstring>
#include <istream>
#include <ostream>
#include <string>

#include "xsp_config.h"		// trim()

namespace xsp {
namespace server {


// All three come from the VERSIONS file at the top of the repository by way of
// cmake/version.cmake, so there is one place to change a version and no way for
// the build and build.py to end up claiming different things. The fallbacks are
// only for a compiler invoked by hand, outside the build system.
const char* const kServerName = "xspeccy";
#ifdef XSP_VERSION
const char* const kServerVersion = XSP_VERSION;
#else
const char* const kServerVersion = "0.0-adhoc";
#endif
// The Xpeccy the binary was actually built against, and the one it was meant to
// be. They differ when a tree was passed in with -DXPECCY_SRC, and a bug report
// that says which is worth a great deal more than one that does not.
#ifdef XSP_XPECCY_VERSION
const char* const kXpeccyVersion = XSP_XPECCY_VERSION;
#else
const char* const kXpeccyVersion = "unknown";
#endif
#ifdef XSP_XPECCY_REQUIRED
const char* const kXpeccyPinned = XSP_XPECCY_REQUIRED;
#else
const char* const kXpeccyPinned = "unknown";
#endif

// Sent with the initialize reply. A tool description can say what one tool does;
// it cannot say which of fifty-three to reach for, in what order, or which pairs
// of them answer questions that sound identical and are not. That is what this
// is for, and the protocol has the field precisely so a server can say it once
// instead of repeating it in every description.
//
// Kept short on purpose: it is prepended to the model's context for the whole
// session, so it earns its place by covering the choices that are actually
// costly to get wrong rather than by being complete.
const char* const kInstructions =
	"A ZX Spectrum (and clones) emulator driven as a tool. There is no window and no "
	"real-time throttle: it runs as fast as the host allows, and the same program produces "
	"the same output every time, so a measurement repeats exactly.\n"
	"\n"
	"Starting on a program:\n"
	"1. machine_config picks the model (Pentagon, ZX48K, Scorpion...). Changing it resets.\n"
	"2. load_file takes .sna/.z80/.tap/.trd. load_labels and load_listing take sjasmplus "
	"symbol and listing files; afterwards every address argument accepts a label name, and "
	"step_line/run_to_line/source_at work on source lines.\n"
	"3. Most effects precalculate for seconds before drawing. Run once with "
	"stop_pc at the main loop, then save_snapshot, and load that snapshot for every later "
	"measurement rather than paying for the precalculation again. skip_until does the same "
	"inside one call.\n"
	"\n"
	"Which tool answers which question:\n"
	"- what is on screen: screenshot (a PNG to open and look at), screen_text, screen_attrs\n"
	"- did my change alter the DATA: screen_digest, which hashes screen memory\n"
	"- did my change alter the PICTURE: frame_digest, which hashes the frame as the ULA drew "
	"it. These are different questions whenever timing matters: in multicolour the picture "
	"depends on screen memory AND on when the bank is switched relative to the beam, so "
	"drifted timing repaints the screen while screen_digest does not move at all.\n"
	"- does a frame fit, and what is the headroom: frame_cost\n"
	"- where do the T-states go: profile, and disassemble with t_states for exact counts\n"
	"- what did the code do: set_breakpoint, step/step_over/step_out, trace, read_memory\n"
	"- when in the frame does this code run, and is that stable across frames: beam_log\n"
	"- put the machine at a raster position: run_to_beam\n"
	"- the picture flickers on purpose (gigascreen, flickering multicolour): screenshot and "
	"frame_digest with blend:2 average the last two completed frames, which is the colour a "
	"person watching sees. One frame of a gigascreen is half the picture and looks like "
	"neither half; blend it before judging it, or before deciding it is broken. video_config "
	"makes that the default for every later call.\n"
	"- the program flips between two screens (double buffering, gigascreen): screen_text and "
	"screen_attrs read the page that is on air and report it as page/page_on_air, and "
	"beam_position reports the same as screen_page. Pass page to look at the other one. Do "
	"not reach for read_memory at $5800 instead: bank 5 is mapped at $4000 whatever the ULA "
	"is showing, so that answers for the screen nobody is looking at.\n"
	"Every stop already reports where the beam was, so raster work rarely needs a separate "
	"beam_position call.\n"
	"\n"
	"Cost: run/run_frames/step take budgets and are cheap. record_video and audio_capture "
	"run many frames and are not; prefer a snapshot plus frame_cost or screen_digest when "
	"measuring, and record a video to show a result rather than to find one.";

// The MCP revisions this server has been checked against. The protocol is
// negotiated by the client naming one and the server answering with the one it
// will actually speak, so echoing a version back unread - which is what this
// used to do - is a promise about a revision nobody here has read.
//
// Nothing this server does differs between these three: it serves tools over
// stdio and nothing else. A client asking for something outside the list is
// answered with ours rather than refused, because that is the answer the
// negotiation exists to produce.
static const char* const kKnownProtocols[] = {
	"2024-11-05", "2025-03-26", "2025-06-18", nullptr
};
static const char* const kProtocolVersion = "2024-11-05";

Machine& machine() {
	// A function-local static rather than a namespace-level one: it is built on
	// first use, which is what keeps it from racing the static initialisation of
	// anything that reaches for it.
	static Machine instance;
	return instance;
}

// ------------------------------------------------------------ tool table

static std::vector<Tool> g_tools;

void tool(const char* name, const char* desc, json schema,
	  std::function<json(const json&)> fn) {
	if (!schema.contains("type")) schema["type"] = "object";
	if (!schema.contains("properties")) schema["properties"] = json::object();
	// Said out loud, because the caller on the other end is usually a language
	// model choosing argument names from a schema: telling it that the list is
	// closed is worth more than catching the mistake afterwards, and dispatch()
	// enforces the same thing for the clients that do not look.
	if (!schema.contains("additionalProperties")) schema["additionalProperties"] = false;
	g_tools.push_back(Tool{name, desc, schema, fn});
}

// An argument the tool does not declare is a mistake, not an extra. Ignoring it
// is the worst of the three options available: the tool then answers a question
// nobody asked - screen_text with "pages" instead of "page" reads whichever page
// happens to be on air - and the caller has no way of finding out.
//
// Returns the complaint, or an empty string when everything is recognised.
static std::string unknownArguments(const Tool& t, const json& arguments) {
	if (!arguments.is_object() || arguments.empty()) return "";
	const auto properties = t.schema.find("properties");
	if (properties == t.schema.end() || !properties->is_object()) return "";

	std::vector<std::string> unknown;
	for (auto it = arguments.begin(); it != arguments.end(); ++it) {
		// Protocol metadata rather than an argument; not ours to refuse.
		if (!it.key().empty() && it.key()[0] == '_') continue;
		if (!properties->contains(it.key())) unknown.push_back(it.key());
	}
	if (unknown.empty()) return "";

	std::string msg = t.name + (unknown.size() > 1 ? ": no such arguments " : ": no such argument ");
	for (size_t i = 0; i < unknown.size(); i++) {
		if (i) msg += ", ";
		msg += "'" + unknown[i] + "'";
	}
	std::string accepted;
	for (auto it = properties->begin(); it != properties->end(); ++it) {
		if (!accepted.empty()) accepted += ", ";
		accepted += it.key();
	}
	// Listing them is the point: a caller that guessed wrong can only correct
	// itself if it is told what there was to guess from.
	msg += accepted.empty() ? ". This tool takes no arguments."
			        : ". It takes: " + accepted;
	return msg;
}

const std::vector<Tool>& tools() { return g_tools; }

const Tool* findTool(const std::string& name) {
	for (const Tool& t : g_tools)
		if (t.name == name) return &t;
	return nullptr;
}

// ------------------------------------------------------------ replies

json okResult(const json& payload) {
	return json{
		{"content", json::array({json{{"type", "text"}, {"text", payload.dump(1, '\t')}}})},
		{"isError", false}
	};
}

json errResult(const std::string& msg) {
	return json{
		{"content", json::array({json{{"type", "text"}, {"text", msg}}})},
		{"isError", true}
	};
}

static json result(const json& id, const json& value) {
	return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", value}};
}

static json error(const json& id, int code, const std::string& message) {
	return json{{"jsonrpc", "2.0"}, {"id", id},
		    {"error", {{"code", code}, {"message", message}}}};
}

// ------------------------------------------------------------ dispatch

json dispatch(const json& req) {
	// Everything below reads fields off `req` by name, and nlohmann throws
	// rather than returns a default when the shape is wrong. A throw here used
	// to reach serve(), which logged it to stderr and sent nothing at all - so
	// a client that had sent an id waited for a reply that was never coming.
	// The shape is therefore checked before anything is read from it.
	if (!req.is_object())
		return error(json(nullptr), -32600, "request must be a JSON object");

	const json id = req.contains("id") ? req["id"] : json(nullptr);
	const bool isNotification = !req.contains("id");
	// An id has to be something a reply can carry back. Anything else leaves
	// the client unable to match the answer to its question even if we send one.
	if (!isNotification && !(id.is_string() || id.is_number() || id.is_null()))
		return error(json(nullptr), -32600, "id must be a string, a number or null");

	if (!req.contains("method"))
		return isNotification ? json() : error(id, -32600, "no method");
	if (!req["method"].is_string())
		return isNotification ? json() : error(id, -32600, "method must be a string");
	const std::string method = req["method"].get<std::string>();

	// params is optional, and absent is not the same as malformed: a client
	// that sent a string where an object belongs has made a mistake worth
	// naming rather than one worth treating as "no arguments".
	if (req.contains("params") && !req["params"].is_null() &&
	    !(req["params"].is_object() || req["params"].is_array()))
		return isNotification ? json()
				      : error(id, -32602, "params must be an object or an array");

	if (method == "initialize") {
		std::string proto = kProtocolVersion;
		if (req.contains("params") && req["params"].is_object() &&
		    req["params"].contains("protocolVersion") &&
		    req["params"]["protocolVersion"].is_string()) {
			const std::string asked = req["params"]["protocolVersion"].get<std::string>();
			for (int i = 0; kKnownProtocols[i]; i++)
				if (asked == kKnownProtocols[i]) { proto = asked; break; }
		}
		return result(id, json{
			{"protocolVersion", proto},
			{"capabilities", {{"tools", json::object()}}},
			{"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
			{"instructions", kInstructions}
		});
	}
	if (method == "notifications/initialized" || method == "notifications/cancelled")
		return json();
	if (method == "ping") return result(id, json::object());

	if (method == "tools/list") {
		json list = json::array();
		for (const Tool& t : g_tools)
			list.push_back({{"name", t.name}, {"description", t.description},
					{"inputSchema", t.schema}});
		return result(id, json{{"tools", list}});
	}

	if (method == "tools/call") {
		const json params = (req.contains("params") && req["params"].is_object())
			? req["params"] : json::object();
		if (!params.contains("name") || !params["name"].is_string())
			return result(id, errResult("tools/call needs params.name, a tool name as a string"));
		const std::string name = params["name"].get<std::string>();
		const json arguments = params.contains("arguments") && params["arguments"].is_object()
			? params["arguments"] : json::object();
		const Tool* t = findTool(name);
		if (!t) return result(id, errResult("unknown tool '" + name + "'"));
		const std::string wrong = unknownArguments(*t, arguments);
		if (!wrong.empty()) return result(id, errResult(wrong));
		// A tool that throws is a bad request, not a broken server: the caller
		// gets the reason as the tool's result and the conversation continues.
		try {
			return result(id, okResult(t->fn(arguments)));
		} catch (const std::exception& e) {
			return result(id, errResult("error in " + name + ": " + e.what()));
		}
	}

	if (isNotification) return json();
	return error(id, -32601, "unknown method '" + method + "'");
}

void serve(std::istream& in, std::ostream& out) {
	std::string line;
	while (std::getline(in, line)) {
		line = xsp::trim(line);		// also drops the \r of a CRLF client
		if (line.empty()) continue;
		json req;
		try {
			req = json::parse(line);
		} catch (const std::exception& e) {
			out << error(nullptr, -32700, std::string("parse error: ") + e.what()).dump()
			    << std::endl;
			continue;
		}
		try {
			const json reply = dispatch(req);
			if (!reply.is_null()) out << reply.dump() << std::endl;
		} catch (const std::exception& e) {
			// dispatch() catches what a tool throws; anything reaching here
			// is the server itself failing, and saying so on stderr is all
			// that can honestly be done about it.
			fprintf(stderr, "xspeccy-mcp: %s\n", e.what());
		}
	}
}

} // namespace server
} // namespace xsp
