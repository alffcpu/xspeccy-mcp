// Assembler listings, so debugging can happen in terms of the source the
// programmer wrote rather than addresses.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace xsp {

struct SourceLine {
	int address = 0;
	int line = 0;		// line number inside `file`
	std::string file;
	std::string text;	// the source itself, comments and all
	int bytes = 0;		// how many bytes this line assembled to
};

class Listing {
public:
	// sjasmplus-style listing: "<line> <address> <bytes...> <source>", with
	// "# file <name>" markers for includes. Lines that produced no bytes are
	// skipped - only addresses we can actually stop at are useful here.
	bool load(const std::string& path, std::string& err);
	void clear();

	const SourceLine* atAddress(int adr) const;		// exact match
	const SourceLine* coveringAddress(int adr) const;	// the line this byte belongs to
	bool addressOfLine(int line, int& adr) const;

	// The listing around an address, `before`/`after` source lines each way.
	std::vector<SourceLine> around(int adr, int before, int after) const;

	size_t size() const { return m_byAddress.size(); }
	const std::string& source() const { return m_source; }

private:
	std::map<int, SourceLine> m_byAddress;	// address -> line
	std::map<int, int> m_lineToAddress;	// line number -> first address
	std::vector<SourceLine> m_ordered;
	std::string m_source;
};

} // namespace xsp
