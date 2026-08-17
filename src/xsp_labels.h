// Symbol tables produced by ZX assemblers, so the agent can talk about
// addresses by name instead of by number.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace xsp {

class Labels {
public:
	struct Entry {
		std::string name;
		int address = 0;	// CPU address the label resolves to
		int bank = -1;		// RAM bank it came from; -1 = already a CPU address
	};

	// Understands the formats sjasmplus and friends emit:
	//   FF:8000 name / 05:200E name / :8000 name   (bank:offset, xpeccy -l)
	//   name: EQU 0x8000 / name EQU $8000 / name = 32768
	// In the bank form the number is an offset *inside the bank*, not a CPU
	// address: bank 5 is seen at $4000, bank 2 at $8000, anything else at
	// $C000, and bank FF means "this is a CPU address already". Same mapping
	// Xpeccy's own loader uses (xcore/labels.cpp).
	// Unparseable lines are skipped, not fatal.
	bool load(const std::string& path, std::string& err);
	void clear();

	// The bank:offset -> CPU address mapping described above, exposed because
	// an argument may spell it out by hand rather than come from a file.
	static int mapBankOffset(int bank, int offset);

	const std::string* atAddress(int adr) const;
	bool find(const std::string& name, int& adr) const;
	const Entry* entry(const std::string& name) const;

	// Replaces known label names in an expression with decimal numbers, so the
	// core's assembler (which has no symbol table) can handle "jp main".
	std::string substitute(const std::string& text) const;

	size_t size() const { return m_byAddr.size(); }
	const std::string& source() const { return m_source; }
	std::vector<Entry> all() const;			// by address, one name per address

private:
	void add(const std::string& name, int adr, int bank);

	std::map<int, std::string> m_byAddr;
	std::map<std::string, Entry> m_byName;
	std::string m_source;
};

} // namespace xsp
