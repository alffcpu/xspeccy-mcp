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

add_executable(xspeccy-mcp
	src/xsp_platform.cpp
	src/xsp_config.cpp
	src/xsp_labels.cpp
	src/xsp_listing.cpp
	src/xsp_audio.cpp
	src/xsp_keyboard.cpp
	src/xsp_machine.cpp
	src/xsp_video.cpp
	src/xsp_png.cpp
	src/xsp_record.cpp
	src/xsp_mcp.cpp
)

target_include_directories(xspeccy-mcp PRIVATE
	"${CMAKE_SOURCE_DIR}/src"
	"${CMAKE_SOURCE_DIR}/third_party/json"
	"${XSP_GENERATED_DIR}"
	"${ZLIB_INCLUDE_DIRS}")

target_compile_definitions(xspeccy-mcp PRIVATE
	XSP_XPECCY_SRC="${XPECCY_SRC}"
	XSP_VERSION="${XSP_VERSION}"
	# What the build actually used, which is not always what VERSIONS asks for:
	# a tree can be handed in with -DXPECCY_SRC. Baked in so that a binary can
	# always say what it was built against, rather than it having to be inferred.
	XSP_XPECCY_VERSION="${XSP_XPECCY_FOUND_VERSION}"
	XSP_XPECCY_REQUIRED="${XSP_XPECCY_REQUIRED}")

target_link_libraries(xspeccy-mcp PRIVATE xpeccy_core ${XSP_ZLIB} ${CMAKE_DL_LIBS})

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

install(TARGETS xspeccy-mcp RUNTIME DESTINATION bin)
