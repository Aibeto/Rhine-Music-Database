# glslang, built from source as a build-time shader compiler.
#
# FidelityFX's Vulkan backend compiles its GLSL shaders to SPIR-V with
# glslangValidator, and the SDK only ships a Windows .exe for it. Rhine does not consume
# prebuilt binaries, so glslang is vendored under third_party/glslang and compiled here.
#
# glslang 16.x no longer uses git submodules: its dependencies are pinned in
# third_party/glslang/known_good.json and fetched by update_glslang_sources.py. The
# checked-out tree therefore needs, relative to third_party/glslang/:
#
#   External/spirv-tools                          KhronosGroup/SPIRV-Tools
#   External/spirv-tools/external/spirv-headers   KhronosGroup/SPIRV-Headers
#
# SPIRV-Tools is required, not optional: ffx_shader_compiler passes -Os to glslang, and
# SPIR-V optimisation lives in SPIRV-Tools. Googletest is not needed because tests are
# off.
#
# When the vendored tree or its dependencies are absent, callers fall back to a system
# glslangValidator; see FfxShaderCompiler.cmake. Set RHINE_BUILD_GLSLANG=OFF to skip the
# source build outright.

include_guard(GLOBAL)

set(RHINE_GLSLANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glslang")
option(RHINE_BUILD_GLSLANG "Build glslangValidator from third_party/glslang for FidelityFX shaders" ON)

# Build (or reuse) the vendored glslang. Sets <out_var> to the executable path, or "" when
# the source build is unavailable.
function(rhine_ffx_build_glslang out_var)
    set(${out_var} "" PARENT_SCOPE)

    if(NOT RHINE_BUILD_GLSLANG)
        return()
    endif()

    if(TARGET glslang-standalone)
        set(${out_var} "$<TARGET_FILE:glslang-standalone>" PARENT_SCOPE)
        return()
    endif()

    if(NOT EXISTS "${RHINE_GLSLANG_DIR}/CMakeLists.txt")
        message(STATUS "glslang: third_party/glslang is not checked out; using the system glslangValidator")
        return()
    endif()

    if(NOT EXISTS "${RHINE_GLSLANG_DIR}/External/spirv-tools/CMakeLists.txt"
       OR NOT EXISTS "${RHINE_GLSLANG_DIR}/External/spirv-tools/external/spirv-headers/CMakeLists.txt")
        message(
            STATUS
                "glslang: vendored dependencies are missing (run update_glslang_sources.py or clone the "
                "commits pinned in known_good.json into External/); using the system glslangValidator")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "glslang: Python3 is required to generate extension headers; using the system glslangValidator")
        return()
    endif()

    # Keep the subproject self-contained: no tests, no install rules, no JS glue.
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL ON CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT ON CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(BUILD_WERROR OFF CACHE BOOL "" FORCE)
    set(ALLOW_EXTERNAL_GTEST OFF CACHE BOOL "" FORCE)

    add_subdirectory("${RHINE_GLSLANG_DIR}" "${CMAKE_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)

    if(NOT TARGET glslang-standalone)
        message(STATUS "glslang: glslang-standalone target was not produced; using the system glslangValidator")
        return()
    endif()

    # glslang names the binary `glslang`, but ffx_shader_compiler's flag is -glslangexe and
    # the tool passes the path through verbatim, so the name does not matter.
    set(${out_var} "$<TARGET_FILE:glslang-standalone>" PARENT_SCOPE)
    message(STATUS "glslang: building glslangValidator from ${RHINE_GLSLANG_DIR}")
endfunction()
