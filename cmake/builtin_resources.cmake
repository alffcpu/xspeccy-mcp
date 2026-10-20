# Compiles the files that ship with the core into the binary: the machine
# definitions (res/machines/*.conf), the screen layouts (res/layouts.conf) and,
# under the same XSP_BUILTIN_ROM switch as the 48K image, the shipped roms
# (config/roms/*.rom). Read from the source tree at run time instead, they
# made the executable a different machine depending on whether the tree it was
# built from was still where the build left it: copied elsewhere, a ZX48 came up
# on the default geometry with 71680 T-states a frame, "zx48" and "evo-tsconf"
# were unknown names, and a Pentagon booted the 48K rom.
#
# The definitions and layouts are ~70K of text and go in always: a machine is
# its definition, so there is nothing to opt out of. The roms are 1.8M of other
# people's copyright, which is the call XSP_BUILTIN_ROM already makes for the
# 48K image (see builtin_rom.cmake and THIRD_PARTY_NOTICES.md); with it off the
# binary carries none and looks for them in the source tree at run time.
#
# Generated at build time rather than at configure time. CMake turns 1.8M of
# rom into a C array in about a second and a half, and configure runs on every
# CMakeLists edit - that second and a half would be paid over and over for
# bytes that change once per pin. As a build step it runs when an input changes
# and not otherwise. The output goes to the build directory like the 48K header;
# nothing is copied into this repository.
#
# Sets XSP_RESOURCE_SOURCES, the generated source to compile into xsp_core.
# Include it after builtin_rom.cmake, which owns the XSP_BUILTIN_ROM option.

set(_res_out "${XSP_GENERATED_DIR}/xsp_builtin_data.cpp")
set(_res_script "${CMAKE_CURRENT_LIST_DIR}/embed_resources.cmake")
set(_res_machines "${XPECCY_SRC}/res/machines")
set(_res_layouts "${XPECCY_SRC}/res/layouts.conf")
set(_res_roms "")
if(XSP_BUILTIN_ROM)
	set(_res_roms "${XPECCY_SRC}/config/roms")
endif()

# The script globs for itself; this glob is only the dependency list.
# CONFIGURE_DEPENDS makes the build notice a file added to or removed from
# either directory, which a plain glob would only see at the next configure.
file(GLOB _res_inputs CONFIGURE_DEPENDS "${_res_machines}/*.conf")
if(EXISTS "${_res_layouts}")
	list(APPEND _res_inputs "${_res_layouts}")
endif()
if(_res_roms)
	file(GLOB _res_rom_files CONFIGURE_DEPENDS "${_res_roms}/*.rom")
	list(APPEND _res_inputs ${_res_rom_files})
	list(LENGTH _res_rom_files _res_rom_count)
	message(STATUS "Built-in roms: ${_res_rom_count} from ${_res_roms}")
else()
	message(STATUS "Built-in roms: off; machines will need their roms at run time")
endif()

# What the command is asked to embed, as a file the output depends on. Ninja
# reruns a command whose arguments changed; the Makefile generators do not, so
# switching XSP_BUILTIN_ROM or pointing XPECCY_SRC elsewhere would keep the old
# bytes. configure_file rewrites it only when the text differs, so an unchanged
# configure does not force the step to run again.
set(_res_args "${XSP_GENERATED_DIR}/xsp_builtin_data.args")
file(WRITE "${_res_args}.in"
	"machines=${_res_machines}\nlayouts=${_res_layouts}\nroms=${_res_roms}\n")
configure_file("${_res_args}.in" "${_res_args}" COPYONLY)

add_custom_command(
	OUTPUT "${_res_out}"
	COMMAND "${CMAKE_COMMAND}"
		"-DXSP_OUT=${_res_out}"
		"-DXSP_MACHINES_DIR=${_res_machines}"
		"-DXSP_LAYOUTS=${_res_layouts}"
		"-DXSP_ROMS_DIR=${_res_roms}"
		-P "${_res_script}"
	DEPENDS ${_res_inputs} "${_res_script}" "${_res_args}"
	COMMENT "Embedding the core's machine definitions, layouts and roms"
	VERBATIM)

set(XSP_RESOURCE_SOURCES "${_res_out}")
