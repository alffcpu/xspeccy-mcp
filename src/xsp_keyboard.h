// ZX keyboard as a 8x5 matrix of half-rows. We drive comp->keyb->map directly:
// nothing else touches the matrix in a headless machine.
#pragma once

#include <string>
#include <vector>

extern "C" {
#include "spectrum.h"
}

namespace xsp {
namespace keyboard {

// A key as the matrix sees it. Names are the ones an agent would type:
// "a".."z", "0".."9", "enter", "space", "caps", "symbol", plus the
// shifted combinations the ROM expects ("delete", "edit", quotes, etc.)
struct Key {
	int row = -1;
	int mask = 0;
	bool caps = false;	// needs Caps Shift
	bool symbol = false;	// needs Symbol Shift
};

bool lookup(const std::string& name, Key& key);	// by key name
bool forChar(char c, Key& key);			// by character to type

void press(Computer* comp, const Key& key);
void release(Computer* comp, const Key& key);
void releaseAll(Computer* comp);

std::vector<std::string> names();

} // namespace keyboard
} // namespace xsp
