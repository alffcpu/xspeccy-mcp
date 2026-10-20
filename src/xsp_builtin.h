// The files that ship with the core, compiled into this binary: the machine
// definitions, the screen layouts and - unless XSP_BUILTIN_ROM is off - the
// shipped roms. The bytes are generated into the build directory by
// cmake/builtin_resources.cmake; this header is the whole interface to them.
//
// They stand where the core's own data directory stands for the GUI: below the
// user's configuration, never instead of it. A user file of the same name is
// read first and wins.
#pragma once

#include <cstddef>
#include <string>

namespace xsp {
namespace builtin {

struct File {
	const char* name;		// file name only, as it is in the source tree
	const unsigned char* data;
	size_t size;
};

struct Set {
	const File* files;
	size_t count;

	const File* begin() const { return files; }
	const File* end() const { return files + count; }
	const File* find(const std::string& name) const {
		for (const File& f : *this)
			if (name == f.name) return &f;
		return nullptr;
	}
};

extern const Set kMachines;	// res/machines/*.conf
extern const Set kLayouts;	// res/layouts.conf
extern const Set kRoms;		// config/roms/*.rom - empty with XSP_BUILTIN_ROM off

} // namespace builtin
} // namespace xsp
