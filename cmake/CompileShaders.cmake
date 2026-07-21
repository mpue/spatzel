# CompileShaders.cmake — GLSL -> SPIR-V at build time, once per backend.
#
# The same GLSL source does not produce the same SPIR-V for every target:
# Vulkan GLSL uses descriptor sets and push constants, OpenGL GLSL uses binding
# points and uniform blocks. So each backend gets its own variant, compiled
# with its own target environment and preprocessor defines, and staged into its
# own subdirectory next to the executable. Which subdirectory to read is a
# backend decision made behind the RHI seam.
#
# Supports either glslc (shaderc, ships with the Vulkan SDK) or
# glslangValidator.

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

# fitzel_add_shaders(<target>
#     SUBDIR <name>            staging subdirectory, i.e. the backend's name
#     TARGET_ENV <env>         vulkan1.3 | opengl
#     [DEFINES <macro>...]
#     SOURCES <glsl>...)
function(fitzel_add_shaders TARGET)
    cmake_parse_arguments(ARG "" "TARGET_ENV;SUBDIR" "SOURCES;DEFINES" ${ARGN})

    if(NOT ARG_TARGET_ENV)
        message(FATAL_ERROR "fitzel_add_shaders(${TARGET}): TARGET_ENV is required")
    endif()
    if(NOT ARG_SUBDIR)
        message(FATAL_ERROR "fitzel_add_shaders(${TARGET}): SUBDIR is required")
    endif()

    set(_output_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/${ARG_SUBDIR}")

    set(_defines "")
    foreach(_define IN LISTS ARG_DEFINES)
        list(APPEND _defines "-D${_define}")
    endforeach()

    set(_outputs "")
    foreach(_src IN LISTS ARG_SOURCES)
        get_filename_component(_abs "${_src}" ABSOLUTE)
        get_filename_component(_name "${_src}" NAME)
        set(_out "${_output_dir}/${_name}.spv")

        if(_fitzel_glsl_name STREQUAL "glslc")
            set(_cmd "${FITZEL_GLSL_COMPILER}"
                     "--target-env=${ARG_TARGET_ENV}"
                     ${_defines}
                     "-O"
                     "-MD" "-MF" "${_out}.d"
                     "-o" "${_out}" "${_abs}")
            set(_depfile DEPFILE "${_out}.d")
        elseif(ARG_TARGET_ENV STREQUAL "opengl")
            # glslangValidator spells the OpenGL target as -G, not --target-env.
            set(_cmd "${FITZEL_GLSL_COMPILER}" "-G" "--quiet" ${_defines}
                     "-o" "${_out}" "${_abs}")
            set(_depfile "")
        else()
            set(_cmd "${FITZEL_GLSL_COMPILER}"
                     "--target-env" "${ARG_TARGET_ENV}"
                     "-V" "--quiet" ${_defines}
                     "-o" "${_out}" "${_abs}")
            set(_depfile "")
        endif()

        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}"
            COMMAND ${_cmd}
            DEPENDS "${_abs}"
            ${_depfile}
            COMMENT "SPIR-V ${ARG_SUBDIR}/${_name}"
            VERBATIM)

        list(APPEND _outputs "${_out}")
    endforeach()

    add_custom_target(${TARGET} DEPENDS ${_outputs})
    set_property(TARGET ${TARGET} PROPERTY FITZEL_SHADER_DIR "${_output_dir}")
    set_property(TARGET ${TARGET} PROPERTY FITZEL_SHADER_SUBDIR "${ARG_SUBDIR}")
endfunction()

# Copies a variant's .spv files into <exe dir>/shaders/<subdir>/.
function(fitzel_stage_shaders EXE SHADER_TARGET)
    get_property(_dir TARGET ${SHADER_TARGET} PROPERTY FITZEL_SHADER_DIR)
    get_property(_subdir TARGET ${SHADER_TARGET} PROPERTY FITZEL_SHADER_SUBDIR)
    add_dependencies(${EXE} ${SHADER_TARGET})
    add_custom_command(TARGET ${EXE} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_dir}" "$<TARGET_FILE_DIR:${EXE}>/shaders/${_subdir}"
        COMMENT "Staging ${_subdir} SPIR-V next to ${EXE}"
        VERBATIM)
endfunction()
