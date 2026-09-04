// The JSON-RPC layer, which until now could only be exercised by starting a
// process and talking to it down a pipe. Everything here is about the shape of
// a reply rather than about the emulator: a tool that throws has to come back as
// a result the caller can read, a notification must produce no reply at all, and
// an unknown method must be an error rather than silence.
#include "unit.h"
#include "xsp_server.h"

#include <sstream>
#include <string>

using namespace xsp;
using namespace xsp::server;

namespace {

json request(const char* method, const json& params = json(), const json& id = 1) {
	json r{{"jsonrpc", "2.0"}, {"method", method}};
	if (!id.is_null()) r["id"] = id;
	if (!params.is_null()) r["params"] = params;
	return r;
}

json call(const char* name, const json& arguments = json::object()) {
	return request("tools/call", json{{"name", name}, {"arguments", arguments}});
}

// The text a tool result carries, which is where both an answer and an error
// end up: MCP has no separate error channel for a tool that ran and refused.
std::string resultText(const json& reply) {
	if (!reply.contains("result")) return "";
	const json& r = reply["result"];
	if (!r.contains("content") || !r["content"].is_array() || r["content"].empty()) return "";
	return r["content"][0].value("text", "");
}

bool isToolError(const json& reply) {
	return reply.contains("result") && reply["result"].value("isError", false);
}

} // namespace

void test_server() {
	unit::begin("server");

	// Two tools that exist only for this suite: one that answers and one that
	// refuses. Registering them here is the whole reason the registry is a
	// function rather than a list built at start-up.
	tool("unit_echo", "gives back what it was given",
	     json{{"properties", {
		     {"n", {{"type", "integer"}}},
		     {"s", {{"type", "string"}}}
	     }}},
	     [](const json& a) { return json{{"got", a}}; });
	tool("unit_refuse", "always refuses", json::object(),
	     [](const json&) -> json { throw std::runtime_error("no thank you"); });

	// tool() fills in the parts of a schema every tool would otherwise repeat,
	// so that a tool declaring only its properties still advertises an object,
	// and says out loud that the list of arguments is closed.
	{
		const Tool* refuse = findTool("unit_refuse");
		CHECK(refuse != nullptr);
		if (refuse) {
			CHECK_EQ(refuse->schema.value("type", ""), std::string("object"));
			CHECK(refuse->schema.contains("properties"));
			CHECK(refuse->schema["properties"].is_object());
			CHECK_EQ(refuse->schema.value("additionalProperties", true), false);
		}
		CHECK(findTool("unit_nothing") == nullptr);
		CHECK(findTool("") == nullptr);
	}

	// ---- initialize ---------------------------------------------------------
	//
	// The protocol version is echoed rather than chosen: the client names the
	// version it speaks and a server that answers with its own favourite is the
	// reason handshakes fail.
	{
		json reply = dispatch(request("initialize",
					      json{{"protocolVersion", "2025-06-18"}}));
		CHECK(reply.contains("result"));
		CHECK_EQ(reply["result"].value("protocolVersion", ""), std::string("2025-06-18"));
		CHECK_EQ(reply["result"]["serverInfo"].value("name", ""), std::string("xspeccy"));
		CHECK(!reply["result"]["serverInfo"].value("version", "").empty());
		CHECK(reply["result"]["capabilities"].contains("tools"));
		// The guidance the server sends once instead of repeating it in fifty
		// descriptions. An empty one is a build that lost it.
		CHECK(reply["result"].value("instructions", "").size() > 200);
		CHECK_EQ(reply.value("id", -1), 1);
		CHECK_EQ(reply.value("jsonrpc", ""), std::string("2.0"));

		// A client that says nothing about the version gets the default rather
		// than a crash on a missing field.
		json bare = dispatch(request("initialize"));
		CHECK(!bare["result"].value("protocolVersion", "").empty());
		json wrongType = dispatch(request("initialize",
						  json{{"protocolVersion", 5}}));
		CHECK(!wrongType["result"].value("protocolVersion", "").empty());

		// A revision this server has not been checked against is answered with
		// one it has, which is what the negotiation is for. Echoing it back
		// unread - which this used to do - is a promise about a document
		// nobody here has read.
		json unknown = dispatch(request("initialize",
						json{{"protocolVersion", "2099-01-01"}}));
		CHECK(unknown["result"].value("protocolVersion", "") != "2099-01-01");
		CHECK(!unknown["result"].value("protocolVersion", "").empty());
	}

	// ---- the housekeeping methods -------------------------------------------
	{
		json pong = dispatch(request("ping"));
		CHECK(pong.contains("result"));
		CHECK(pong["result"].is_object());

		// Notifications take no reply, and answering one is a protocol error
		// that some clients treat as a stray message.
		CHECK(dispatch(request("notifications/initialized", json(), json())).is_null());
		CHECK(dispatch(request("notifications/cancelled", json(), json())).is_null());
	}

	// ---- tools/list ---------------------------------------------------------
	{
		json list = dispatch(request("tools/list"));
		CHECK(list.contains("result"));
		const json& all = list["result"]["tools"];
		CHECK(all.is_array());
		CHECK_EQ((int)all.size(), (int)tools().size());
		bool sawEcho = false;
		for (const json& t : all) {
			// Every entry has all three fields: a client that indexes tools
			// by name and shows the description needs both, and an absent
			// schema makes a tool uncallable.
			CHECK(t.contains("name"));
			CHECK(t.contains("description"));
			CHECK(t.contains("inputSchema"));
			CHECK(!t.value("description", "").empty());
			if (t.value("name", "") == "unit_echo") sawEcho = true;
		}
		CHECK(sawEcho);
	}

	// ---- tools/call ---------------------------------------------------------
	{
		json reply = dispatch(call("unit_echo", json{{"n", 7}}));
		CHECK(reply.contains("result"));
		CHECK(!isToolError(reply));
		// The payload comes back as text, because that is the only thing the
		// content array carries.
		CHECK(resultText(reply).find("\"n\"") != std::string::npos);
		CHECK(resultText(reply).find("7") != std::string::npos);

		// Arguments are optional, and a call without them is a call with none
		// rather than a failure.
		json noArgs = dispatch(request("tools/call", json{{"name", "unit_echo"}}));
		CHECK(!isToolError(noArgs));

		// So is a call whose arguments are not an object: it is a bad request,
		// and treating it as no arguments lets the tool answer for itself.
		json badArgs = dispatch(request("tools/call",
						json{{"name", "unit_echo"}, {"arguments", 5}}));
		CHECK(!isToolError(badArgs));
	}

	// ---- an argument the tool does not know ----------------------------------
	//
	// Refused rather than ignored. The caller is usually a language model
	// generating JSON, so a plausible wrong name is the normal case, and a tool
	// that quietly drops it answers a question nobody asked.
	{
		json reply = dispatch(call("unit_echo", json{{"nn", 7}}));
		CHECK(isToolError(reply));
		CHECK(resultText(reply).find("'nn'") != std::string::npos);
		CHECK(resultText(reply).find("no such argument") != std::string::npos);
		// The names it could have used, because a caller that guessed wrong can
		// only correct itself if it is told what there was to guess from.
		CHECK(resultText(reply).find("n, s") != std::string::npos);

		// Several at once are listed together rather than one call at a time.
		json two = dispatch(call("unit_echo", json{{"nn", 1}, {"ss", 2}}));
		CHECK(isToolError(two));
		CHECK(resultText(two).find("no such arguments") != std::string::npos);
		CHECK(resultText(two).find("'nn'") != std::string::npos);
		CHECK(resultText(two).find("'ss'") != std::string::npos);

		// A tool that takes none says so, rather than listing an empty set.
		json none = dispatch(call("unit_refuse", json{{"why", "because"}}));
		CHECK(isToolError(none));
		CHECK(resultText(none).find("takes no arguments") != std::string::npos);

		// A partly-right call is still refused: the good arguments do not
		// excuse the bad one, because acting on half a request is the thing
		// being prevented.
		json mixed = dispatch(call("unit_echo", json{{"n", 1}, {"nope", 2}}));
		CHECK(isToolError(mixed));

		// Protocol metadata is not an argument and is not ours to refuse.
		json meta = dispatch(call("unit_echo", json{{"n", 1}, {"_meta", json::object()}}));
		CHECK(!isToolError(meta));
	}

	// A tool that throws is a bad request, not a broken server: the reason
	// reaches the caller as a tool error and the conversation continues.
	{
		json reply = dispatch(call("unit_refuse"));
		CHECK(reply.contains("result"));
		CHECK(!reply.contains("error"));
		CHECK(isToolError(reply));
		CHECK(resultText(reply).find("no thank you") != std::string::npos);
		// Naming the tool matters when several were called in one turn.
		CHECK(resultText(reply).find("unit_refuse") != std::string::npos);
	}

	// A tool nobody registered is an error the caller can act on, and it names
	// what was asked for so a typo is visible.
	{
		json reply = dispatch(call("unit_does_not_exist"));
		CHECK(isToolError(reply));
		CHECK(resultText(reply).find("unit_does_not_exist") != std::string::npos);
		CHECK(resultText(reply).find("unknown tool") != std::string::npos);
	}

	// ---- methods that are not ours ------------------------------------------
	{
		json reply = dispatch(request("resources/list"));
		CHECK(reply.contains("error"));
		CHECK_EQ(reply["error"].value("code", 0), -32601);
		CHECK(reply["error"].value("message", "").find("resources/list") != std::string::npos);

		// The same thing as a notification: nothing to reply to, so nothing is
		// said. Answering an unknown notification is how a client ends up with
		// a reply it has no request for.
		CHECK(dispatch(request("resources/list", json(), json())).is_null());
	}

	// ---- valid JSON that is not a valid request ------------------------------
	//
	// Every field below is read off the request by name, and the JSON library
	// throws rather than defaulting when the shape is wrong. Those throws used
	// to reach the read loop, which logged them to stderr and sent nothing at
	// all - so a client that had sent an id waited for an answer that was never
	// coming. Each of these has to produce a reply.
	{
		// Not an object at all.
		json array = dispatch(json::array({1, 2}));
		CHECK(array.contains("error"));
		CHECK_EQ(array["error"].value("code", 0), -32600);
		CHECK(dispatch(json(42)).contains("error"));
		CHECK(dispatch(json("hello")).contains("error"));
		CHECK(dispatch(json()).contains("error"));

		// An object, but the method is not a name.
		json numeric = dispatch(json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", 5}});
		CHECK(numeric.contains("error"));
		CHECK_EQ(numeric["error"].value("code", 0), -32600);
		// and the id comes back, or the client cannot match the answer
		CHECK_EQ(numeric.value("id", -1), 7);

		json noMethod = dispatch(json{{"jsonrpc", "2.0"}, {"id", 8}});
		CHECK(noMethod.contains("error"));
		CHECK_EQ(noMethod.value("id", -1), 8);

		// params of the wrong shape is a mistake worth naming rather than one
		// worth reading as "no arguments".
		json badParams = dispatch(json{{"jsonrpc", "2.0"}, {"id", 9},
					       {"method", "tools/call"}, {"params", "nonsense"}});
		CHECK(badParams.contains("error"));
		CHECK_EQ(badParams["error"].value("code", 0), -32602);

		// A call with no tool name, or one that is not a name.
		json noName = dispatch(json{{"jsonrpc", "2.0"}, {"id", 10},
					    {"method", "tools/call"}, {"params", json::object()}});
		CHECK(isToolError(noName));
		CHECK(resultText(noName).find("params.name") != std::string::npos);
		json numericName = dispatch(json{{"jsonrpc", "2.0"}, {"id", 11},
						 {"method", "tools/call"},
						 {"params", {{"name", 5}}}});
		CHECK(isToolError(numericName));

		// The same shapes as notifications: nothing to reply to, so nothing is
		// said, and still no throw.
		CHECK(dispatch(json{{"jsonrpc", "2.0"}, {"method", 5}}).is_null());
		CHECK(dispatch(json{{"jsonrpc", "2.0"}}).is_null());
		CHECK(dispatch(json{{"jsonrpc", "2.0"}, {"method", "tools/call"},
				    {"params", 5}}).is_null());
	}

	// ---- the stdio loop ------------------------------------------------------
	//
	// serve() is the only part that touches streams, which is what lets the rest
	// of this file exist. Here it is fed a whole conversation at once.
	{
		std::istringstream in(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n"
			"\n"					// blank lines are skipped
			"   \n"
			"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\r\n"	// a CRLF client
			"not json at all\n"
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}\n");
		std::ostringstream out;
		serve(in, out);

		// One line per reply, and the notification produced none.
		std::vector<std::string> lines;
		{
			std::istringstream split(out.str());
			std::string line;
			while (std::getline(split, line)) if (!line.empty()) lines.push_back(line);
		}
		CHECK_EQ((int)lines.size(), 4);
		if (lines.size() == 4) {
			CHECK(json::parse(lines[0]).value("id", -1) == 1);
			CHECK(json::parse(lines[1]).value("id", -1) == 2);
			// A line that is not JSON is answered rather than dropped, so a
			// client that sent rubbish is told instead of waiting forever.
			json broken = json::parse(lines[2]);
			CHECK(broken.contains("error"));
			CHECK_EQ(broken["error"].value("code", 0), -32700);
			CHECK(broken["id"].is_null());
			// And the stream carries on: one bad line does not end the session.
			CHECK(json::parse(lines[3]).value("id", -1) == 3);
		}
	}

	// An empty stream is a client that hung up before saying anything, which is
	// an ordinary end and not an error.
	{
		std::istringstream in("");
		std::ostringstream out;
		serve(in, out);
		CHECK(out.str().empty());
	}
}
