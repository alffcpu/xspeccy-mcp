#include "xsp_labels.h"
#include "xsp_config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace xsp {

static bool isIdentChar(char c) {
	return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '?';
}

// "0x8000", "$8000", "#8000", "8000h", "32768", "%1010"
static bool parseValue(const std::string& raw, int& out) {
	std::string s = trim(raw);
	if (s.empty()) return false;
	int base = 10;
	size_t i = 0;
	if (s.compare(0, 2, "0x") == 0 || s.compare(0, 2, "0X") == 0) { base = 16; i = 2; }
	else if (s[0] == '$' || s[0] == '#') { base = 16; i = 1; }
	else if (s[0] == '%') { base = 2; i = 1; }
	else if (s.size() > 1 && (s.back() == 'h' || s.back() == 'H')) { base = 16; s.pop_back(); }
	if (i >= s.size()) return false;
	char* end = nullptr;
	long v = strtol(s.c_str() + i, &end, base);
	if (!end || *end != '\0') return false;
	out = (int)v;
	return true;
}

static bool isHexWord(const std::string& s) {
	if (s.empty()) return false;
	for (char c : s) if (!isxdigit((unsigned char)c)) return false;
	return true;
}

// "BB:OOOO name" holds an offset inside RAM bank BB, not a CPU address - the
// assembler wrote it while that bank was paged in somewhere. Map it back the
// way Xpeccy's own loader does (xcore/labels.cpp): 128K machines keep bank 5
// at $4000 and bank 2 at $8000 permanently, every other bank is only ever seen
// through the window at $C000. Bank FF means the number is already a CPU address.
int Labels::mapBankOffset(int bank, int offset) {
	if (bank == 0xff) return offset & 0xffff;
	offset &= 0x3fff;
	switch (bank) {
		case 5: return offset | 0x4000;
		case 2: return offset | 0x8000;
		default: return offset | 0xc000;
	}
}

void Labels::clear() {
	m_byAddr.clear();
	m_byName.clear();
	m_source.clear();
}

void Labels::add(const std::string& name, int adr, int bank) {
	adr &= 0xffff;
	if (!m_byAddr.count(adr)) m_byAddr[adr] = name;		// first name wins
	m_byName[name] = Entry{name, adr, bank};
}

bool Labels::load(const std::string& path, std::string& err) {
	std::ifstream f(path);
	if (!f.is_open()) { err = "can't open '" + path + "'"; return false; }

	clear();
	std::string line;
	while (std::getline(f, line)) {
		size_t cmt = line.find(';');
		if (cmt != std::string::npos) line = line.substr(0, cmt);
		line = trim(line);
		if (line.empty()) continue;

		std::string name;
		int adr = -1;
		int bank = -1;

		size_t colon = line.find(':');
		// "FF:8000 name" / "05:200E name" / ":8000 name" - bank, offset, name
		if (colon != std::string::npos && colon <= 2) {
			std::string bankTxt = trim(line.substr(0, colon));
			std::string rest = trim(line.substr(colon + 1));
			size_t sp = rest.find_first_of(" \t");
			if (sp != std::string::npos) {
				std::string offTxt = rest.substr(0, sp);
				std::string nm = trim(rest.substr(sp));
				// both fields must be hex, or "hl: EQU 5" would look like a bank
				if (!nm.empty() && isHexWord(offTxt) &&
				    (bankTxt.empty() || isHexWord(bankTxt))) {
					int b = bankTxt.empty() ? 0xff
						: (int)strtol(bankTxt.c_str(), nullptr, 16);
					adr = Labels::mapBankOffset(b, (int)strtol(offTxt.c_str(), nullptr, 16));
					bank = (b == 0xff) ? -1 : b;
					name = nm;
				}
			}
		}

		// "name: EQU value" / "name EQU value" / "name = value"
		if (adr < 0) {
			size_t i = 0;
			while (i < line.size() && isIdentChar(line[i])) i++;
			if (i > 0) {
				std::string nm = line.substr(0, i);
				std::string rest = trim(line.substr(i));
				if (!rest.empty() && rest[0] == ':') rest = trim(rest.substr(1));
				if (rest.size() > 2 && (rest.compare(0, 3, "EQU") == 0 || rest.compare(0, 3, "equ") == 0))
					rest = trim(rest.substr(3));
				else if (!rest.empty() && rest[0] == '=')
					rest = trim(rest.substr(1));
				else
					rest.clear();
				int v;
				if (!rest.empty() && parseValue(rest, v)) { adr = v & 0xffff; name = nm; }
			}
		}

		if (adr < 0 || name.empty()) continue;
		add(name, adr, bank);
	}

	m_source = path;
	if (m_byAddr.empty()) {
		err = "no labels recognised in '" + path + "'";
		return false;
	}
	return true;
}

const std::string* Labels::atAddress(int adr) const {
	auto it = m_byAddr.find(adr & 0xffff);
	return it == m_byAddr.end() ? nullptr : &it->second;
}

bool Labels::find(const std::string& name, int& adr) const {
	auto it = m_byName.find(name);
	if (it == m_byName.end()) return false;
	adr = it->second.address;
	return true;
}

const Labels::Entry* Labels::entry(const std::string& name) const {
	auto it = m_byName.find(name);
	return it == m_byName.end() ? nullptr : &it->second;
}

std::string Labels::substitute(const std::string& text) const {
	if (m_byName.empty()) return text;
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	while (i < text.size()) {
		if (!isIdentChar(text[i]) || isdigit((unsigned char)text[i])) {
			out += text[i++];
			continue;
		}
		size_t j = i;
		while (j < text.size() && isIdentChar(text[j])) j++;
		std::string word = text.substr(i, j - i);
		auto it = m_byName.find(word);
		if (it != m_byName.end()) out += std::to_string(it->second.address);
		else out += word;
		i = j;
	}
	return out;
}

std::vector<Labels::Entry> Labels::all() const {
	std::vector<Entry> res;
	res.reserve(m_byAddr.size());
	for (const auto& kv : m_byAddr) {
		auto it = m_byName.find(kv.second);
		res.push_back(it == m_byName.end() ? Entry{kv.second, kv.first, -1} : it->second);
	}
	return res;
}

} // namespace xsp
