#include "xsp_listing.h"
#include "xsp_config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace xsp {

static bool isHexPair(const std::string& s) {
	if (s.size() != 2) return false;
	return isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]);
}

static bool allHex(const std::string& s) {
	if (s.empty()) return false;
	for (char c : s)
		if (!isxdigit((unsigned char)c)) return false;
	return true;
}

void Listing::clear() {
	m_byAddress.clear();
	m_lineToAddress.clear();
	m_ordered.clear();
	m_source.clear();
}

bool Listing::load(const std::string& path, std::string& err) {
	std::ifstream f(path);
	if (!f.is_open()) { err = "can't open '" + path + "'"; return false; }

	clear();
	std::string raw, file;
	while (std::getline(f, raw)) {
		// "# file main.asm" / "# file closed main.asm" markers around includes
		if (!raw.empty() && raw[0] == '#') {
			size_t pos = raw.find("file");
			if (pos != std::string::npos) {
				std::string rest = trim(raw.substr(pos + 4));
				if (rest.compare(0, 6, "closed") == 0) rest = trim(rest.substr(6));
				if (!rest.empty()) file = rest;
			}
			continue;
		}

		// <line no> <address> <bytes...> <source>
		size_t i = 0;
		while (i < raw.size() && isspace((unsigned char)raw[i])) i++;
		size_t numStart = i;
		while (i < raw.size() && isdigit((unsigned char)raw[i])) i++;
		if (i == numStart) continue;
		int lineNo = atoi(raw.substr(numStart, i - numStart).c_str());
		while (i < raw.size() && (raw[i] == '+' || raw[i] == '~')) i++;	// macro/repeat markers
		if (i < raw.size() && !isspace((unsigned char)raw[i])) continue;
		while (i < raw.size() && isspace((unsigned char)raw[i])) i++;

		size_t adrStart = i;
		while (i < raw.size() && isxdigit((unsigned char)raw[i])) i++;
		std::string adrTok = raw.substr(adrStart, i - adrStart);
		if (adrTok.size() < 4 || adrTok.size() > 8 || !allHex(adrTok)) continue;
		int address = (int)strtol(adrTok.c_str(), nullptr, 16) & 0xffff;

		// emitted bytes, if any
		int byteCount = 0;
		size_t save = i;
		while (true) {
			size_t j = i;
			while (j < raw.size() && isspace((unsigned char)raw[j])) j++;
			if (j + 1 >= raw.size()) break;
			std::string tok = raw.substr(j, 2);
			if (!isHexPair(tok)) break;
			// a hex pair immediately followed by more word characters is source
			if (j + 2 < raw.size() && !isspace((unsigned char)raw[j + 2])) break;
			byteCount++;
			i = j + 2;
		}
		if (byteCount == 0) { i = save; continue; }	// no code on this line

		std::string text = trim(raw.substr(i));
		if (text.empty()) continue;

		SourceLine sl;
		sl.address = address;
		sl.line = lineNo;
		sl.file = file;
		sl.text = text;
		sl.bytes = byteCount;

		if (!m_byAddress.count(address)) m_byAddress[address] = sl;
		if (!m_lineToAddress.count(lineNo)) m_lineToAddress[lineNo] = address;
		m_ordered.push_back(sl);
	}

	m_source = path;
	if (m_byAddress.empty()) {
		err = "no code lines recognised in '" + path + "'";
		return false;
	}
	return true;
}

const SourceLine* Listing::atAddress(int adr) const {
	auto it = m_byAddress.find(adr & 0xffff);
	return it == m_byAddress.end() ? nullptr : &it->second;
}

const SourceLine* Listing::coveringAddress(int adr) const {
	adr &= 0xffff;
	auto it = m_byAddress.upper_bound(adr);
	if (it == m_byAddress.begin()) return nullptr;
	--it;
	if (adr < it->second.address + it->second.bytes) return &it->second;
	return nullptr;
}

bool Listing::addressOfLine(int line, int& adr) const {
	auto it = m_lineToAddress.find(line);
	if (it == m_lineToAddress.end()) return false;
	adr = it->second;
	return true;
}

std::vector<SourceLine> Listing::around(int adr, int before, int after) const {
	std::vector<SourceLine> res;
	const SourceLine* here = coveringAddress(adr);
	if (!here) return res;
	// find it in source order, then walk out both ways
	size_t idx = 0;
	bool found = false;
	for (size_t i = 0; i < m_ordered.size(); i++) {
		if (m_ordered[i].address == here->address && m_ordered[i].line == here->line) {
			idx = i;
			found = true;
			break;
		}
	}
	if (!found) { res.push_back(*here); return res; }
	size_t from = (size_t)before > idx ? 0 : idx - before;
	size_t to = idx + after + 1;
	if (to > m_ordered.size()) to = m_ordered.size();
	for (size_t i = from; i < to; i++) res.push_back(m_ordered[i]);
	return res;
}

} // namespace xsp
