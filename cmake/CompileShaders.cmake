# CompileShaders.cmake — GLSL -> SPIR-V at build time.
#
# Supports either glslc (shaderc, ships with the Vulkan SDK) or
# glslangValidator. Compiled modules land next to the executable in
# <runtime-dir>/shaders/<name>.spv and are loaded as raw binaries at runtime.

find_program(FITZEL_GLSL_COMPILER
    NAMES glslc glslangValidator
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES bin Bin
    DOC "GLSL -> SPIR-V compiler")

if(NOT FITZEL_GLSL_COMPILER)
    message(FATAL_ERROR
        "No GLSL compiler found. Install the Vulkan SDK (provides glslc) or "
        "put glslangValidator on PATH.")
endif()

get_filename_component(_fitzel_glsl_name "${FITZEL_GLSL_COMPILER}" NAME_WE)
message(STATUS "Shader compiler: ${FITZEL_GLSL_COMPILER}")

# fitzel_add_shaders(<target> SOURCES <glsl>... [TARGET_ENV vulkan1.3])
#
# Creates a custom target `<target>` that all shaders depend on, and makes the
# .spv files land in the runtime output directory of `<consumer>` targets that
# link it. Callers add a dependency from their executable to `<target>`.
function(fitzel_add_shaders TARGET)
    cmake_parse_arguments(ARG "" "TARGET_ENV;OUTPUT_DIR" "SOURCES" ${ARGN})

    if(NOT ARG_TARGET_ENV)
        set(ARG_TARGET_ENV "vulkan1.3")
    endif()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    endif()

    set(_outputs "")
    foreach(_src IN LISTS ARG_SOURCES)
        get_filename_component(_abs "${_src}" ABSOLUTE)
        get_filename_component(_name "${_src}" NAME)
        set(_out "${ARG_OUTPUT_DIR}/${_name}.spv")

        if(_fitzel_glsl_name STREQUAL "glslc")
            set(_cmd "${FITZEL_GLSL_COMPILER}"
                     "--target-env=${ARG_TARGET_ENV}"
                     "-O"
                     "-MD" "-MF" "${_out}.d"
                     "-o" "${_out}" "${_abs}")
            set(_depfile DEPFILE "${_out}.d")
        else()
            set(_cmd "${FITZEL_GLSL_COMPILER}"
                     "--target-env" "${ARG_TARGET_ENV}"
                     "-V" "--quiet"
                     "-o" "${_out}" "${_abs}")
            set(_depfile "")
        endif()

        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${ARG_OUTPUT_DIR}"
            COMMAND ${_cmd}
            DEPENDS "${_abs}"
            ${_depfile}
            COMMENT "SPIR-V ${_name}"
            VERBATIM)

        list(APPEND _outputs "${_out}")
    endforeach()

    add_custom_target(${TARGET} DEPENDS ${_outputs})
    set_property(TARGET ${TARGET} PROPERTY FITZEL_SHADER_DIR "${ARG_OUTPUT_DIR}")
endfunction()

# Copies the built .spv files next to an executable so it can load them with a
# path relative to its own location.
function(fitzel_stage_shaders EXE SHADER_TARGET)
    get_property(_dir TARGET ${SHADER_TARGET} PROPERTY FITZEL_SHADER_DIR)
    add_dependencies(${EXE} ${SHADER_TARGET})
    add_custom_command(TARGET ${EXE} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_dir}" "$<TARGET_FILE_DIR:${EXE}>/shaders"
        COMMENT "Staging SPIR-V next to ${EXE}"
        VERBATIM)
endfunction()
