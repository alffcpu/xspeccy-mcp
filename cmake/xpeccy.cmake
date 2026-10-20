# Builds the upstream Xpeccy emulator core (src/libxpeccy, pure C) as a static
# library from an EXTERNAL source tree. The upstream repository is never copied
# into this repo and never modified - we only compile it.
#
# Point the build at a Xpeccy source tree with either:
#   cmake -DXPECCY_SRC=/path/to/Xpeccy-0.6.20260804 ...
#   XPECCY_SRC=/path/to/Xpeccy-0.6.20260804 cmake ...
#
# The oldest release that compiles is XPECCY_MINIMUM in VERSIONS: it reshaped
# Computer's status bits into sysflag[], dropped the hwTab[] declaration and
# stopped clearing the register bunch. Older trees fail to compile rather than
# misbehave. XPECCY_VERSION there is the release actually built against, which
# build.py reads as well; a tree that is not it still builds, with a warning
# below. Both numbers are read from that file rather than written out again.

if(NOT XPECCY_SRC AND DEFINED ENV{XPECCY_SRC})
	set(XPECCY_SRC "$ENV{XPECCY_SRC}")
endif()

if(NOT XPECCY_SRC)
	# usual local checkouts, newest first; xpeccy-plus trees count too
	file(GLOB _xp_candidates
		"$ENV{HOME}/xpeccy/Xpeccy-*"
		"$ENV{HOME}/Xpeccy-*"
		"$ENV{HOME}/xpeccy-plus*"
		"$ENV{HOME}/Xpeccy-plus*"
		"${CMAKE_SOURCE_DIR}/../Xpeccy-*"
		"${CMAKE_SOURCE_DIR}/../xpeccy/Xpeccy-*"
		"${CMAKE_SOURCE_DIR}/../xpeccy-plus*"
		"${CMAKE_SOURCE_DIR}/../Xpeccy-plus*")
	list(SORT _xp_candidates)
	list(REVERSE _xp_candidates)
	# "Newest first" is a sort of directory NAMES, so anything alphabetic wins
	# over a version: xpeccy-plus-main and Xpeccy-win both sort after a bare
	# version. Collect first, choose after.
	# Same marker as the hard floor below, and it has to stay the same one: a
	# tree this loop accepts is a tree the build will then try to compile, so a
	# looser test here only moves the failure to somewhere that does not explain
	# itself. The marker is comp_set_cont in spectrum.h - the contention call
	# the adapter makes, which only the xpeccy-plus core has. An upstream
	# samstyle tree fails the probe now, which is deliberate: with the move to
	# the fork it can no longer build this adapter anyway.
	foreach(_c ${_xp_candidates})
		if(EXISTS "${_c}/src/libxpeccy/spectrum.h")
			file(READ "${_c}/src/libxpeccy/spectrum.h" _xp_probe)
			if(_xp_probe MATCHES "comp_set_cont")
				list(APPEND _xp_usable "${_c}")
			else()
				# Not an xpeccy-plus tree. Skipping it rather than picking it
				# and failing matters when both are on disk, which is the normal
				# state of a machine that has an upstream checkout lying around.
				list(APPEND _xp_too_old "${_c}")
			endif()
		endif()
	endforeach()
	unset(_xp_probe)
	# A released tree beats a working checkout: the version in the name says what
	# you get, while Xpeccy-git is whatever it was last left at. A bare version
	# beats a suffixed one for the same reason - Xpeccy-0.6.20260804-win next to
	# Xpeccy-0.6.20260804 is somebody's patched copy, and the emulator's GUI does
	# need patching to build on Windows, so that pair is a normal thing to find.
	foreach(_xp_pat "^Xpeccy-[0-9][0-9.]*$" "^Xpeccy-[0-9]")
		if(NOT XPECCY_SRC)
			foreach(_c ${_xp_usable})
				get_filename_component(_xp_name "${_c}" NAME)
				if(_xp_name MATCHES "${_xp_pat}")
					set(XPECCY_SRC "${_c}")
					break()
				endif()
			endforeach()
		endif()
	endforeach()
	unset(_xp_pat)
	if(NOT XPECCY_SRC AND _xp_usable)
		list(GET _xp_usable 0 XPECCY_SRC)
	endif()
	unset(_xp_name)
endif()

if(NOT XPECCY_SRC OR NOT EXISTS "${XPECCY_SRC}/src/libxpeccy/spectrum.h")
	set(_xp_hint "")
	if(_xp_too_old)
		string(REPLACE ";" "\n    " _xp_old_list "${_xp_too_old}")
		set(_xp_hint "Found, but not xpeccy-plus (${XSP_XPECCY_MINIMUM} is the floor), skipped:\n    ${_xp_old_list}\n")
	endif()
	message(FATAL_ERROR
		"Xpeccy sources not found.\n"
		"${_xp_hint}"
		"Pass -DXPECCY_SRC=/path/to/xpeccy-plus (the directory holding src/libxpeccy).\n"
		"Or run: python build.py\n"
		"Sources are built in place and never modified.")
endif()

# A tree that cannot build this adapter, caught here rather than as a screenful
# of C++ errors about functions that do not exist. Since the move to xpeccy-plus
# the floor is the fork: spectrum.h there declares comp_set_cont(), the call
# applyMachineDef() makes for a definition's contmem key, and no upstream
# samstyle tree has it. The probe is for the name rather than a version stamp
# because the call being there is precisely what compilation needs.
file(READ "${XPECCY_SRC}/src/libxpeccy/spectrum.h" _xp_spectrum_h)
if(NOT _xp_spectrum_h MATCHES "comp_set_cont")
	message(FATAL_ERROR
		"The Xpeccy at ${XPECCY_SRC} is not xpeccy-plus.\n"
		"VERSIONS pins ${XSP_XPECCY_REQUIRED}, which is what this is built and\n"
		"tested against. Run python build.py to fetch it.\n"
		"An upstream samstyle tree no longer compiles this adapter.")
endif()
unset(_xp_spectrum_h)

set(XPECCY_SRC "${XPECCY_SRC}" CACHE PATH "Path to the Xpeccy source tree" FORCE)
set(XPECCY_INC "${XPECCY_SRC}/src")
message(STATUS "Xpeccy core: ${XPECCY_SRC}")

# Which release this actually is, read off the directory name, and whether it is
# the one VERSIONS pins. The vidPage probe above is only a floor: it accepts the
# pinned release and everything after it alike, so a newer tree left on disk used
# to be picked up without a word. That is how a build stops matching the one the
# tests were written against, and the failure it produces later looks like a bug
# in the server rather than a different emulator underneath it.
#
# A warning and not an error, deliberately. Building against a patched or a newer
# tree on purpose is a legitimate thing to do - the emulator's own GUI needs
# patching to compile on Windows, so a `-win` copy next to the plain one is
# normal - and the version is baked into the binary either way, so any report
# says which tree it came from.
get_filename_component(_xp_dir "${XPECCY_SRC}" NAME)
if(EXISTS "${XPECCY_SRC}/VERSION")
	# xpeccy-plus carries its release in a VERSION file, not the dir name - and
	# it comes first, so a checkout downloaded as Xpeccy-<n>-plus still reads
	# right. The recorded pin is that version with "-plus" appended.
	file(STRINGS "${XPECCY_SRC}/VERSION" _xp_ver LIMIT_COUNT 1)
	string(STRIP "${_xp_ver}" _xp_ver)
	set(XSP_XPECCY_FOUND_VERSION "${_xp_ver}-plus")
	unset(_xp_ver)
elseif(_xp_dir MATCHES "^Xpeccy-([0-9][0-9.]*[0-9])")
	set(XSP_XPECCY_FOUND_VERSION "${CMAKE_MATCH_1}")
else()
	set(XSP_XPECCY_FOUND_VERSION "unknown (${_xp_dir})")
endif()

if(NOT XSP_XPECCY_FOUND_VERSION STREQUAL XSP_XPECCY_REQUIRED)
	message(WARNING
		"Xpeccy version mismatch.\n"
		"  VERSIONS pins: ${XSP_XPECCY_REQUIRED}\n"
		"  building against: ${XSP_XPECCY_FOUND_VERSION}\n"
		"  from: ${XPECCY_SRC}\n"
		"The build will go ahead and the binary will report the tree it used, but "
		"the test suite's expected values were measured against the pinned release. "
		"Run python build.py to fetch it, or pass "
		"-DXPECCY_SRC=/path/to/Xpeccy-${XSP_XPECCY_REQUIRED}, or update VERSIONS if "
		"the move is intended.")
endif()
unset(_xp_dir)

# The plus core is not pure C any more: sound/ym-2203.cpp and the ymfm library
# under it are C++ and are part of the chip table unconditionally.
file(GLOB_RECURSE XPECCY_CORE_SOURCES "${XPECCY_SRC}/src/libxpeccy/*.c")
file(GLOB_RECURSE XPECCY_CORE_CXX "${XPECCY_SRC}/src/libxpeccy/*.cpp")
list(LENGTH XPECCY_CORE_SOURCES _xp_count)
list(LENGTH XPECCY_CORE_CXX _xp_cxx_count)
message(STATUS "Xpeccy core: ${_xp_count} C files, ${_xp_cxx_count} C++ files")

add_library(xpeccy_core STATIC ${XPECCY_CORE_SOURCES} ${XPECCY_CORE_CXX})

target_include_directories(xpeccy_core PUBLIC
	"${XPECCY_SRC}/src"
	"${XPECCY_SRC}/src/libxpeccy")

set_target_properties(xpeccy_core PROPERTIES C_STANDARD 99 C_EXTENSIONS ON
	CXX_STANDARD 17 CXX_EXTENSIONS OFF)

include(TestBigEndian)
test_big_endian(XPECCY_BIG_ENDIAN)
if(XPECCY_BIG_ENDIAN)
	target_compile_definitions(xpeccy_core PUBLIC WORDS_BIG_ENDIAN)
else()
	target_compile_definitions(xpeccy_core PUBLIC WORDS_LITTLE_ENDIAN)
endif()

# Upstream warnings are not ours to fix, and -O0 fails to link: the GameBoy core
# declares lr_swaph() `inline` without `static`, so at -O0 no out-of-line copy is
# emitted. Force at least -O1 regardless of the build type.
#
# The core is written in GNU C (the upstream build passes -std=gnu99), so on
# Windows use MinGW-w64 rather than MSVC.
if(MSVC)
	target_compile_options(xpeccy_core PRIVATE /w /O2)
else()
	target_compile_options(xpeccy_core PRIVATE -w -O2)
endif()

# GCC 14 turned a family of old C looseness into hard errors, and -w does not
# reach them because they are errors rather than warnings. The core trips one of
# them on Windows only: cpu.c assigns the FARPROC that GetProcAddress() returns
# straight to a cpuCore*(*)() (via the dlsym shim), which is
# -Wincompatible-pointer-types. -fpermissive puts that whole family back to
# warnings, where -w then silences it, and is the only way to compile upstream on
# a current toolchain without editing it.
# (GCC 13 and older neither need it nor accept it for C, so ask for the version.)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
	target_compile_options(xpeccy_core PRIVATE -fpermissive)
endif()
