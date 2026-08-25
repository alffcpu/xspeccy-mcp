# Reads the VERSIONS file at the top of the repository. Anything that needs a
# version number gets it from here, so build.py and the CMake build cannot end
# up disagreeing about which Xpeccy the server is meant to be built against.

set(XSP_VERSIONS_FILE "${CMAKE_SOURCE_DIR}/VERSIONS")
if(NOT EXISTS "${XSP_VERSIONS_FILE}")
	message(FATAL_ERROR "VERSIONS is missing from ${CMAKE_SOURCE_DIR}.\n"
		"It carries the server version and the pinned Xpeccy release, and both "
		"the CMake build and build.py read it.")
endif()

# Editing VERSIONS has to re-run CMake. Without this, `cmake --build` after a
# version bump relinks the old number in silence, and the binary reports a
# version that is not the one in the file it is supposed to come from - which
# was the whole point of having one file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${XSP_VERSIONS_FILE}")

# KEY = VALUE, # starts a comment. Deliberately dull to parse: this file is read
# by Python as well, and a format only one of them understands is a format that
# will be edited wrongly.
function(xsp_read_version key out)
	file(STRINGS "${XSP_VERSIONS_FILE}" _lines REGEX "^[ \t]*${key}[ \t]*=")
	if(NOT _lines)
		message(FATAL_ERROR "VERSIONS has no ${key}")
	endif()
	list(GET _lines 0 _line)
	string(REGEX REPLACE "^[ \t]*${key}[ \t]*=[ \t]*" "" _value "${_line}")
	string(REGEX REPLACE "#.*$" "" _value "${_value}")
	string(STRIP "${_value}" _value)
	if(_value STREQUAL "")
		message(FATAL_ERROR "VERSIONS has an empty ${key}")
	endif()
	set(${out} "${_value}" PARENT_SCOPE)
endfunction()

xsp_read_version(XSPECCY_MCP_VERSION XSP_VERSION)
xsp_read_version(XPECCY_VERSION XSP_XPECCY_REQUIRED)
xsp_read_version(XPECCY_MINIMUM XSP_XPECCY_MINIMUM)

message(STATUS "xspeccy-mcp ${XSP_VERSION}, built against Xpeccy ${XSP_XPECCY_REQUIRED}")
