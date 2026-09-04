cmake_minimum_required(VERSION 3.16)
project(xspeccy-mcp C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(version)		# must precede xpeccy: it sets the version to check against
include(xpeccy)
include(builtin_rom)

# A binary you can copy to another machine and run. On MinGW that means not
# depending on the toolchain's DLLs (libgcc, libstdc++, zlib1), which live in the
# MSYS2 tree and are not there on a plain Windows box; elsewhere a fully static
# link is not the norm, so ask for it explicitly if you want it.
if(MINGW)
	set(_xsp_static_default ON)
else()
	set(_xsp_static_default OFF)
endif()
option(XSP_STATIC "Link libgcc/libstdc++/zlib into the executable" ${_xsp_static_default})
if(XSP_STATIC AND NOT MSVC)
	set(ZLIB_USE_STATIC_LIBS ON)	# honoured by FindZLIB from CMake 3.24
endif()

find_package(ZLIB REQUIRED)		# PNG encoding

set(XSP_ZLIB ZLIB::ZLIB)
if(XSP_STATIC AND NOT MSVC AND CMAKE_VERSION VERSION_LESS 3.24)
	# older FindZLIB ignores ZLIB_USE_STATIC_LIBS and hands us the import
	# library, which would put zlib1.dll back into the dependencies
	get_filename_component(_zdir "${ZLIB_LIBRARY}" DIRECTORY)
	find_library(XSP_ZLIB_STATIC NAMES libz.a HINTS "${_zdir}")
	if(XSP_ZLIB_STATIC)
		set(XSP_ZLIB "${XSP_ZLIB_STATIC}")
	else()
		message(WARNING "no static zlib beside ${ZLIB_LIBRARY} - the binary will need zlib at run time")
	endif()
endif()

# The modules under src/ are the parts; src/tools/ is one file per group of
# tools, all of them registered through src/xsp_server.cpp.
set(XSP_CORE_SOURCES
	src/xsp_platform.cpp
	src/xsp_config.cpp
	src/xsp_labels.cpp
	src/xsp_listing.cpp
	src/xsp_audio.cpp
	src/xsp_keyboard.cpp
	src/xsp_machine.cpp
	src/xsp_blend.cpp
	src/xsp_settings.cpp
	src/xsp_video.cpp
	src/xsp_png.cpp
	src/xsp_record.cpp
	src/xsp_args.cpp
	src/xsp_server.cpp
)

set(XSP_TOOL_SOURCES
	src/tools/common.cpp
	src/tools/machine.cpp
	src/tools/execution.cpp
	src/tools/memory.cpp
	src/tools/debug.cpp
	src/tools/video.cpp
	src/tools/source.cpp
	src/tools/timing.cpp
	src/tools/audio.cpp
	src/tools/input.cpp
	src/tools/media.cpp
)

# The parts, built once and linked into both the server and the test binary.
# They used to be listed in both targets and compiled twice, which is a minute
# of everyone's time per clean build and two places to forget a file.
add_library(xsp_core OBJECT ${XSP_CORE_SOURCES})

target_include_directories(xsp_core PUBLIC
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/third_party/json"
	"${XSP_GENERATED_DIR}"
	"${XPECCY_SRC}/src"
	"${XPECCY_SRC}/src/libxpeccy"
	"${ZLIB_INCLUDE_DIRS}")

target_compile_definitions(xsp_core PUBLIC
	XSP_XPECCY_SRC="${XPECCY_SRC}"
	XSP_VERSION="${XSP_VERSION}"
	# What the build actually used, which is not always what VERSIONS asks for:
	# a tree can be handed in with -DXPECCY_SRC. Baked in so that a binary can
	# always say what it was built against, rather than it having to be inferred.
	XSP_XPECCY_VERSION="${XSP_XPECCY_FOUND_VERSION}"
	XSP_XPECCY_REQUIRED="${XSP_XPECCY_REQUIRED}")

if(MSVC)
	target_compile_options(xsp_core PRIVATE /W3)
else()
	target_compile_options(xsp_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(xspeccy-mcp
	${XSP_TOOL_SOURCES}
	src/xsp_mcp.cpp
	$<TARGET_OBJECTS:xsp_core>
)

target_link_libraries(xspeccy-mcp PRIVATE xsp_core xpeccy_core ${XSP_ZLIB} ${CMAKE_DL_LIBS})

if(XSP_STATIC AND NOT MSVC)
	target_link_options(xspeccy-mcp PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(WIN32)
	target_link_libraries(xspeccy-mcp PRIVATE winmm)	# upstream links it too
else()
	target_link_libraries(xspeccy-mcp PRIVATE m)
endif()

if(MSVC)
	target_compile_options(xspeccy-mcp PRIVATE /W3)
else()
	target_compile_options(xspeccy-mcp PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Unit tests. The end-to-end suite in tests/smoke.sh drives the finished server
# over stdio, which is the right way to check that the parts are wired together
# and a poor way to check the parts. These cover the logic underneath it: the
# colour arithmetic, the 128K bank map, the listing lookups, the PNG and GIF
# writers, the keyboard matrix, the settings table and the configuration reader.
#
# Every module in XSP_CORE_SOURCES is linked; only src/xsp_mcp.cpp and the tool
# registrations are left out, the first because it owns main() and the second
# because registering every tool would pull the whole emulator into a binary
# meant to test the parts underneath it. An earlier version linked just the modules that never mention Computer,
# which made a tidy claim and a bad test set: it left the keyboard map, the WAV
# writer and the whole settings table untestable for a reason that had nothing to
# do with how much they were worth testing. tools/coverage.py measures what is
# actually reached, which is the honest version of the same question.
option(XSP_UNIT_TESTS "Build the unit test binary" ON)
if(XSP_UNIT_TESTS)
	add_executable(xspeccy-unit EXCLUDE_FROM_ALL
		$<TARGET_OBJECTS:xsp_core>
		tests/unit/unit.cpp
		tests/unit/main.cpp
		tests/unit/test_args.cpp
		tests/unit/test_audio.cpp
		tests/unit/test_blend.cpp
		tests/unit/test_config.cpp
		tests/unit/test_gif.cpp
		tests/unit/test_keyboard.cpp
		tests/unit/test_labels.cpp
		tests/unit/test_listing.cpp
		tests/unit/test_machine.cpp
		tests/unit/test_platform.cpp
		tests/unit/test_png.cpp
		tests/unit/test_server.cpp
		tests/unit/test_settings.cpp
	)
	target_link_libraries(xspeccy-unit PRIVATE xsp_core xpeccy_core ${XSP_ZLIB} ${CMAKE_DL_LIBS})
	if(WIN32)
		target_link_libraries(xspeccy-unit PRIVATE winmm)
	else()
		target_link_libraries(xspeccy-unit PRIVATE m)
	endif()
	if(MSVC)
		target_compile_options(xspeccy-unit PRIVATE /W3)
	else()
		target_compile_options(xspeccy-unit PRIVATE -Wall -Wextra -Wno-unused-parameter)
	endif()

	# A test file that is on disk and not in the two lists above is a suite
	# nobody runs, and it fails silently and forever: the build says nothing,
	# the binary says nothing, and the coverage table just looks a little worse
	# than it should. Adding one means editing three places - the sources here,
	# the declaration in unit.h and the call in main.cpp - so the build checks
	# that the first and the last were not forgotten.
	#
	# A glob is normally the wrong way to list sources, because CMake does not
	# re-run when a file appears. Here it is not the list; it is the check on
	# the list, and a stale glob only delays the complaint to the next configure.
	file(GLOB _xsp_test_files RELATIVE "${CMAKE_SOURCE_DIR}" "tests/unit/test_*.cpp")
	file(READ "${CMAKE_SOURCE_DIR}/tests/unit/main.cpp" _xsp_test_main)
	get_target_property(_xsp_unit_sources xspeccy-unit SOURCES)
	foreach(_test ${_xsp_test_files})
		if(NOT "${_test}" IN_LIST _xsp_unit_sources)
			message(FATAL_ERROR
				"${_test} exists but is not in xspeccy-unit's sources: it would "
				"never be compiled. Add it to CMakeLists.txt.")
		endif()
		get_filename_component(_suite "${_test}" NAME_WE)
		if(NOT _xsp_test_main MATCHES "${_suite}\\(\\)")
			message(FATAL_ERROR
				"${_test} is compiled but ${_suite}() is never called from "
				"tests/unit/main.cpp: it would never run.")
		endif()
	endforeach()

	enable_testing()
	add_test(NAME unit COMMAND xspeccy-unit)
endif()

install(TARGETS xspeccy-mcp RUNTIME DESTINATION bin)
