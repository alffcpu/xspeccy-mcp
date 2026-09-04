// The JSON-RPC side of the server: the tool registry, and turning one request
// into one reply.
//
// Deliberately separate from the tools themselves and from stdio. dispatch()
// takes a request and returns the reply rather than printing it, which is what
// makes the protocol testable: a wrong method, an unknown tool or a tool that
// throws are all things to check, and none of them should need a process and a
// pipe to check.
#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "json.hpp"

#include "xsp_args.h"
#include "xsp_machine.h"

namespace xsp {
namespace server {

// Reported over MCP and by --version; all of them come from the VERSIONS file
// by way of cmake/version.cmake.
extern const char* const kServerName;
extern const char* const kServerVersion;
extern const char* const kXpeccyVersion;
extern const char* const kXpeccyPinned;
extern const char* const kInstructions;

// The one machine this process is serving. A single global is the honest shape
// here: the protocol is one conversation over one pipe, and a second machine
// would have nobody to talk to.
Machine& machine();

struct Tool {
	std::string name;
	std::string description;
	json schema;
	std::function<json(const json&)> fn;
};

void tool(const char* name, const char* desc, json schema,
	  std::function<json(const json&)> fn);
const std::vector<Tool>& tools();
const Tool* findTool(const std::string& name);

// How a tool's return value reaches the caller.
json okResult(const json& payload);
json errResult(const std::string& msg);

// The reply to one request, or a null json for a notification that takes none.
json dispatch(const json& request);

// Reads line-delimited JSON-RPC from `in` and writes one reply per line to
// `out`, until the input ends.
void serve(std::istream& in, std::ostream& out);

} // namespace server
} // namespace xsp
