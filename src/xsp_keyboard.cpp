#include "xsp_keyboard.h"

#include <cctype>
#include <cstring>
#include <map>

namespace xsp {
namespace keyboard {

// (row, mask) exactly as libxpeccy/input.c keyTab has them: map[row] bit set =
// key up, cleared = key down. row 0 is the half-row selected by A15.
struct Cell { char id; int row; int mask; };

static const Cell kMatrix[] = {
	{'1',4,1},{'2',4,2},{'3',4,4},{'4',4,8},{'5',4,16},
	{'6',3,16},{'7',3,8},{'8',3,4},{'9',3,2},{'0',3,1},
	{'q',5,1},{'w',5,2},{'e',5,4},{'r',5,8},{'t',5,16},
	{'y',2,16},{'u',2,8},{'i',2,4},{'o',2,2},{'p',2,1},
	{'a',6,1},{'s',6,2},{'d',6,4},{'f',6,8},{'g',6,16},
	{'h',1,16},{'j',1,8},{'k',1,4},{'l',1,2},{'\n',1,1},	// Enter
	{'^',7,1},{'z',7,2},{'x',7,4},{'c',7,8},{'v',7,16},	// ^ = Caps Shift
	{'b',0,16},{'n',0,8},{'m',0,4},{'!',0,2},{' ',0,1},	// ! = Symbol Shift
	{0,0,0}
};

static bool cell(char id, Key& key) {
	for (int i = 0; kMatrix[i].id; i++) {
		if (kMatrix[i].id == id) {
			key.row = kMatrix[i].row;
			key.mask = kMatrix[i].mask;
			return true;
		}
	}
	return false;
}

// name -> base key + shifts
struct Named { const char* name; char id; bool caps; bool symbol; };

static const Named kNamed[] = {
	{"enter", '\n', false, false}, {"return", '\n', false, false},
	{"space", ' ', false, false},
	{"caps", '^', false, false}, {"capsshift", '^', false, false},
	{"symbol", '!', false, false}, {"symbolshift", '!', false, false},
	{"delete", '0', true, false}, {"backspace", '0', true, false},
	{"edit", '1', true, false},
	{"capslock", '2', true, false},
	{"truevideo", '3', true, false}, {"invvideo", '4', true, false},
	{"left", '5', true, false}, {"down", '6', true, false},
	{"up", '7', true, false}, {"right", '8', true, false},
	{"graph", '9', true, false},
	{"break", ' ', true, false},
	{"extend", '^', false, true},		// Caps+Symbol together
	{nullptr, 0, false, false}
};

// characters that need Symbol Shift, and which key they sit on
static const char* kSymbolPairs[] = {
	"!1", "@2", "#3", "$4", "%5", "&6", "'7", "(8", ")9", "_0",
	"-j", "+k", "=l", ":z", ";o", "\"p", ",n", ".m", "/v", "*b",
	"<r", ">t", "?c", "^h", "[y", "]u", nullptr
};

bool lookup(const std::string& rawName, Key& key) {
	std::string name;
	for (char c : rawName) name += (char)tolower((unsigned char)c);

	for (int i = 0; kNamed[i].name; i++) {
		if (name == kNamed[i].name) {
			if (!cell(kNamed[i].id, key)) return false;
			key.caps = kNamed[i].caps;
			key.symbol = kNamed[i].symbol;
			return true;
		}
	}
	if (name.size() == 1) return forChar(rawName[0], key);
	return false;
}

bool forChar(char c, Key& key) {
	key = Key();
	if (c == '\r') c = '\n';
	if (isupper((unsigned char)c)) {
		if (!cell((char)tolower((unsigned char)c), key)) return false;
		key.caps = true;
		return true;
	}
	if (cell(c, key)) {
		// '^' and '!' are the shift keys themselves in kMatrix, not characters
		if (c == '^' || c == '!') { key = Key(); }
		else return true;
	}
	for (int i = 0; kSymbolPairs[i]; i++) {
		if (kSymbolPairs[i][0] == c) {
			if (!cell(kSymbolPairs[i][1], key)) return false;
			key.symbol = true;
			return true;
		}
	}
	return false;
}

static Key shiftKey(bool caps) {
	Key k;
	cell(caps ? '^' : '!', k);
	return k;
}

void press(Computer* comp, const Key& key) {
	if (key.row < 0) return;
	if (key.caps)   { Key s = shiftKey(true);  comp->keyb->map[s.row] &= ~s.mask; }
	if (key.symbol) { Key s = shiftKey(false); comp->keyb->map[s.row] &= ~s.mask; }
	comp->keyb->map[key.row] &= ~key.mask;
}

void release(Computer* comp, const Key& key) {
	if (key.row < 0) return;
	comp->keyb->map[key.row] |= key.mask;
	if (key.caps)   { Key s = shiftKey(true);  comp->keyb->map[s.row] |= s.mask; }
	if (key.symbol) { Key s = shiftKey(false); comp->keyb->map[s.row] |= s.mask; }
}

void releaseAll(Computer* comp) {
	for (int i = 0; i < 8; i++) comp->keyb->map[i] = 0xff;
	memset(comp->keyb->matrix, 0, sizeof(comp->keyb->matrix));
}

std::vector<std::string> names() {
	std::vector<std::string> res;
	for (int i = 0; kNamed[i].name; i++) res.push_back(kNamed[i].name);
	return res;
}

} // namespace keyboard
} // namespace xsp
