# FidelityFX shader permutation compiler, built from SDK sources at build time.
#
# Upstream ffx_shader_compiler is a Windows tool: it includes <Windows.h>, <pathcch.h>
# and the DXC COM interfaces, and the SDK only ships prebuilt Windows binaries under
# libs/. Rhine never consumes prebuilt binaries, so this module compiles the tool from
# the vendored sources instead. Only the GLSL/glslang code path is needed (the Vulkan
# backend never asks for HLSL), so:
#
#   * cmake/ffx_sc_port/Windows.h implements the handful of Win32 path/file helpers that
#     the GLSL path reaches, keeping paths in the SDK's backslash form internally and
#     converting at the filesystem boundary.
#   * cmake/ffx_sc_port/pch.hpp replaces the SDK pch, pointing the COM/D3D includes at
#     empty stubs.
#   * cmake/ffx_sc_port/hlsl_compiler.h + hlsl_compiler_stub.cpp keep HLSLCompiler's
#     public surface (ffx_sc.cpp selects a compiler by name) but throw if ever used.
#   * cmake/ffx_sc_port/entry.cpp owns main() and widens argv for the tool's wmain.
#
# The staged copy under the build tree is what gets compiled: quoted includes resolve
# next to the translation unit first, so the port headers reliably shadow the upstream
# ones without patching anything under FidelityFX-SDK/.
#
# glslangValidator is a host toolchain dependency (like the C++ compiler itself). The
# SDK's bundled copy is a Windows .exe, so on Linux the system binary is used; point
# RHINE_FFX_GLSLANG at a different one to override.

include_guard(GLOBAL)

set(RHINE_FFX_SC_PORT_SRC "${CMAKE_CURRENT_LIST_DIR}/ffx_sc_port")
set(RHINE_FFX_SC_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffx_sc_port")

set(RHINE_FFX_GLSLANG "" CACHE FILEPATH "Path to glslangValidator used for FidelityFX Vulkan shaders")

# Stage the upstream sources next to the port shims so quoted includes resolve to the
# port headers. Everything lands flat in one directory, mirroring upstream's src/.
#
# Copy a source while applying literal replacements, given as (old new old new ...) pairs.
# Fails loudly when a pattern is gone, so an SDK update that fixes an issue upstream cannot
# silently leave us patching nothing.
function(_rhine_ffx_sc_stage_patched source output_path)
    file(READ "${source}" contents)
    # 支持一次打多组补丁：参数成对出现 (old_text new_text ...)。
    math(EXPR _pair_count "${ARGC} / 2 - 1")
    if(_pair_count LESS 1)
        message(FATAL_ERROR "FidelityFX: _rhine_ffx_sc_stage_patched 至少需要一组补丁")
    endif()
    math(EXPR _last_pair "${_pair_count} - 1")
    foreach(_i RANGE 0 ${_last_pair})
        math(EXPR _arg_idx "${_i} * 2 + 2")
        math(EXPR _next_idx "${_arg_idx} + 1")
        set(old_text "${ARGV${_arg_idx}}")
        set(new_text "${ARGV${_next_idx}}")
        string(FIND "${contents}" "${old_text}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "FidelityFX: expected to patch '${old_text}' in ${source}, but it is not there")
        endif()
        string(REPLACE "${old_text}" "${new_text}" contents "${contents}")
    endforeach()
    file(WRITE "${output_path}" "${contents}")
endfunction()

function(_rhine_ffx_sc_stage_sources sdk_dir)
    set(stage "${RHINE_FFX_SC_STAGE_DIR}/src")
    file(MAKE_DIRECTORY "${stage}")

    set(upstream_tools "${sdk_dir}/sdk/tools/ffx_shader_compiler")

    # Upstream translation units and headers that compile unmodified.
    foreach(rel src/ffx_sc.cpp src/compiler.h src/glsl_compiler.h src/utils.h)
        get_filename_component(name "${rel}" NAME)
        configure_file("${upstream_tools}/${rel}" "${stage}/${name}" COPYONLY)
    endforeach()

    # glsl_compiler.cpp needs two upstream bugs patched.
    #
    # (1) Its glslang output parser calls token.pop_back() unconditionally, and an empty
    #     token (which happens whenever a pipe read delivers a chunk that is just "\n")
    #     makes size()-1 wrap to SIZE_MAX, so libstdc++ throws std::length_error from
    #     basic_string::_M_create. Single shaders usually avoid it; a parallel build hits
    #     it every time.
    #
    # (2) MD5HashString() writes the digest with a fixed size argument:
    #         char out[33];
    #         for (i < 16) { std::snprintf(out_ptr, 32, "%02x", sig[i]); out_ptr += 2; }
    #     The bound stays 32 while the pointer walks forward, so the last iterations tell
    #     snprintf there are 32 writable bytes where only 1 remains -> 1-byte stack
    #     overflow. It happens to survive on glibc builds without fortification, but
    #     Ubuntu 24.04 enables _FORTIFY_SOURCE=3 by default and the check aborts the tool
    #     with "*** buffer overflow detected ***", failing every shader permutation.
    #     Passing the actually-remaining room keeps the same output and fits the buffer.
    _rhine_ffx_sc_stage_patched(
        "${upstream_tools}/src/glsl_compiler.cpp" "${stage}/glsl_compiler.cpp"
        "                token.pop_back();"
        "                if (!token.empty())\n                    token.pop_back();"
        "        std::snprintf(out_ptr, 32, \"%02x\", sig[i]);"
        "        std::snprintf(out_ptr, static_cast<size_t>(out + sizeof(out) - out_ptr), \"%02x\", sig[i]);")

    # Port replacements. pch.hpp and hlsl_compiler.h intentionally shadow the upstream
    # headers of the same name; the rest are Win32/D3D stubs that upstream expects to
    # come from the platform SDK.
    foreach(name pch.hpp hlsl_compiler.h DXBCChecksum.h Windows.h pathcch.h
                 atlcomcli.h dxcapi.h d3dcompiler.h d3d12shader.h)
        configure_file("${RHINE_FFX_SC_PORT_SRC}/${name}" "${stage}/${name}" COPYONLY)
    endforeach()

    # Port-owned translation units: portable utils, HLSL stub, wide-argv entry point.
    foreach(name utils.cpp hlsl_compiler_stub.cpp entry.cpp)
        configure_file("${RHINE_FFX_SC_PORT_SRC}/${name}" "${stage}/${name}" COPYONLY)
    endforeach()
endfunction()

# Windows 分支：直接编上游源码，不再用 cmake/ffx_sc_port/ 的影子头。
#
# 为什么不像 Linux 那样整套 stage：Windows 上 pch.hpp 需要的 <Windows.h>、
# <atlcomcli.h>（ATL）、<dxcapi.h> 都是真的，影子头没有存在的理由；上游的
# hlsl_compiler.cpp 也必须真编进来 —— DX12 的 HLSL 置换正是靠它驱动 DXC。
# 唯一还要 stage 的是 glsl_compiler.cpp：上游那处 token.pop_back() 在空 token 上同样会
# 越界（MSVC Debug 直接断言、Release 下 size 回绕），所以照样打补丁再编。
function(_rhine_ffx_sc_build_windows sdk_dir out_var)
    set(${out_var} "" PARENT_SCOPE)

    if(NOT MSVC)
        # atlcomcli.h 只有 MSVC 工具链自带；MinGW / 裸 clang-cl 环境没有 ATL。
        message(FATAL_ERROR
            "FidelityFX: Windows 上从源码构建 ffx_shader_compiler 需要 MSVC 工具链"
            "（上游 pch.hpp 依赖 ATL 的 atlcomcli.h）。请从 Developer PowerShell 配置构建。")
    endif()

    set(tools "${sdk_dir}/sdk/tools/ffx_shader_compiler")
    set(stage "${RHINE_FFX_SC_STAGE_DIR}/win")
    file(MAKE_DIRECTORY "${stage}")

    # 只 stage 需要打补丁的那个 TU，其余直接编上游源码：少一份会漂移的副本。
    # 两处补丁与 Linux 分支相同（空 token 上的 pop_back、MD5Hex 的栈溢出），
    # 见 _rhine_ffx_sc_stage_sources 里对成因的说明。
    _rhine_ffx_sc_stage_patched(
        "${tools}/src/glsl_compiler.cpp" "${stage}/glsl_compiler.cpp"
        "                token.pop_back();"
        "                if (!token.empty())\n                    token.pop_back();"
        "        std::snprintf(out_ptr, 32, \"%02x\", sig[i]);"
        "        std::snprintf(out_ptr, static_cast<size_t>(out + sizeof(out) - out_ptr), \"%02x\", sig[i]);")

    add_executable(ffx_sc
        "${tools}/src/ffx_sc.cpp"
        "${stage}/glsl_compiler.cpp"
        "${tools}/src/hlsl_compiler.cpp"
        "${tools}/src/DXBCChecksum.cpp"
        "${tools}/src/utils.cpp"
        "${tools}/src/pch.cpp"
        "${tools}/libs/MD5/md5.cpp"
        "${tools}/libs/SPIRV-Reflect/spirv_reflect.c"
        "${tools}/libs/tiny-process-library/process.cpp"
        "${tools}/libs/tiny-process-library/process_win.cpp")

    target_include_directories(ffx_sc PRIVATE
        "${stage}"
        "${tools}/src"
        "${tools}/libs/MD5"
        "${tools}/libs/SPIRV-Reflect"
        "${tools}/libs/tiny-process-library"
        # 上游把 DXC 的 dxcapi.h 和 Agility SDK 头一起签在 libs/ 下：这里只取头文件，
        # 不碰它们自带的预编译 DLL（那正是要摆脱的东西）。
        "${tools}/libs/dxc/inc"
        "${tools}/libs/agilitysdk/include")

    target_compile_features(ffx_sc PRIVATE cxx_std_17)
    set_target_properties(ffx_sc PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${RHINE_FFX_SC_STAGE_DIR}/bin")

    # 上游源码里有非 ASCII 文本（帮助字符串），显式按 UTF-8 编，别让它按本地代码页
    # 解析后报 C4819 或截断字符串。
    target_compile_options(ffx_sc PRIVATE /utf-8 /W3)
    # dxguid：DXBC 校验用到的 IID；pathcch：上游用 PathCch* 处理路径。
    target_link_libraries(ffx_sc PRIVATE dxguid pathcch)

    set(${out_var} "$<TARGET_FILE:ffx_sc>" PARENT_SCOPE)
endfunction()

# Build the tool. Sets <out_var> to the executable path, or "" when unsupported.
function(rhine_ffx_build_shader_compiler sdk_dir out_var)
    set(${out_var} "" PARENT_SCOPE)

    # Vulkan 和 DX12 两个后端都会调本函数，目标只建一次。
    if(TARGET ffx_sc)
        set(${out_var} "$<TARGET_FILE:ffx_sc>" PARENT_SCOPE)
        return()
    endif()

    if(WIN32)
        _rhine_ffx_sc_build_windows("${sdk_dir}" _rhine_ffx_sc_windows_exe)
        set(${out_var} "${_rhine_ffx_sc_windows_exe}" PARENT_SCOPE)
        return()
    endif()

    _rhine_ffx_sc_stage_sources("${sdk_dir}")

    set(tools "${sdk_dir}/sdk/tools/ffx_shader_compiler")
    set(stage "${RHINE_FFX_SC_STAGE_DIR}/src")

    add_executable(ffx_sc
        "${stage}/ffx_sc.cpp"
        "${stage}/glsl_compiler.cpp"
        "${stage}/utils.cpp"
        "${stage}/hlsl_compiler_stub.cpp"
        "${stage}/entry.cpp"
        "${tools}/libs/MD5/md5.cpp"
        "${tools}/libs/SPIRV-Reflect/spirv_reflect.c"
        "${tools}/libs/tiny-process-library/process.cpp"
        "${tools}/libs/tiny-process-library/process_unix.cpp")

    target_include_directories(ffx_sc PRIVATE
        "${stage}"
        "${tools}/libs/MD5"
        "${tools}/libs/SPIRV-Reflect"
        "${tools}/libs/tiny-process-library")

    target_compile_features(ffx_sc PRIVATE cxx_std_17)
    set_target_properties(ffx_sc PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${RHINE_FFX_SC_STAGE_DIR}/bin")

    find_package(Threads REQUIRED)
    target_link_libraries(ffx_sc PRIVATE Threads::Threads)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Upstream sources are MSVC-clean but not warning-clean; keep them quiet without
        # hiding real diagnostics behind -Werror.
        target_compile_options(ffx_sc PRIVATE -w -fno-strict-aliasing)
    endif()

    set(${out_var} "$<TARGET_FILE:ffx_sc>" PARENT_SCOPE)
endfunction()

# Resolve the glslangValidator to drive, in order of preference:
#   1. RHINE_FFX_GLSLANG, an explicit override;
#   2. glslang built from third_party/glslang (see Glslang.cmake) — no prebuilt binary;
#   3. the SDK's bundled copy (Windows only);
#   4. the system glslangValidator.
function(rhine_ffx_resolve_glslang sdk_dir out_var)
    if(RHINE_FFX_GLSLANG AND EXISTS "${RHINE_FFX_GLSLANG}")
        set(${out_var} "${RHINE_FFX_GLSLANG}" PARENT_SCOPE)
        return()
    endif()

    if(COMMAND rhine_ffx_build_glslang)
        rhine_ffx_build_glslang(_rhine_ffx_vendored_glslang)
        if(_rhine_ffx_vendored_glslang)
            set(${out_var} "${_rhine_ffx_vendored_glslang}" PARENT_SCOPE)
            return()
        endif()
    endif()

    if(WIN32)
        set(bundled "${sdk_dir}/sdk/tools/ffx_shader_compiler/libs/glslangValidator/bin/x64/glslangValidator.exe")
        if(EXISTS "${bundled}")
            set(${out_var} "${bundled}" PARENT_SCOPE)
            return()
        endif()
    endif()

    find_program(_rhine_ffx_glslang_program NAMES glslangValidator glslang)
    if(_rhine_ffx_glslang_program)
        message(STATUS "FidelityFX: using system glslangValidator at ${_rhine_ffx_glslang_program}")
        set(${out_var} "${_rhine_ffx_glslang_program}" PARENT_SCOPE)
        return()
    endif()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

# Compile every shader in <shader_dir> into <output_dir> permutation headers.
#
# Mirrors upstream's compile_shaders_with_depfile: four header variants per shader
# (wave32 / wave64 / 16-bit / wave64+16-bit), because the SDK's blob accessors include
# all four unconditionally. Arguments follow the upstream contract exactly:
#   ffx_sc <base> <api-base> <permutations> -name=<n> -DFFX_HALF=<0|1>
#          -I<include>... -output=<dir> <shader>
#
# <out_var> receives the list of generated permutation headers.
#
# <extra_args> is prepended to every invocation (tool selection, thread count).
# Anything after <out_var> is passed through to add_custom_command DEPENDS, which is how
# the caller makes the shaders depend on the ffx_sc and glslang targets.
function(rhine_ffx_compile_shaders
         executable
         extra_args
         base_args
         api_base_args
         permutation_args
         include_args
         shader_files
         output_dir
         out_var)

    file(MAKE_DIRECTORY "${output_dir}")

    set(include_flags "")
    foreach(inc IN LISTS include_args)
        list(APPEND include_flags "-I${inc}")
    endforeach()

    set(common_args ${extra_args} ${base_args} ${api_base_args} ${permutation_args})
    set(outputs "")

    foreach(shader IN LISTS shader_files)
        get_filename_component(shader_base "${shader}" NAME_WE)

        # Each variant: <name suffix> <FFX_HALF value>
        foreach(variant IN ITEMS "|0" "_wave64|0" "_16bit|1" "_wave64_16bit|1")
            string(REPLACE "|" ";" variant_parts "${variant}")
            list(GET variant_parts 0 suffix)
            list(GET variant_parts 1 half_value)

            set(header "${output_dir}/${shader_base}${suffix}_permutations.h")
            set(name "${shader_base}${suffix}")

            set(arguments
                ${common_args}
                "-name=${name}"
                "-DFFX_HALF=${half_value}"
                ${include_flags}
                "-output=${output_dir}"
                "${shader}")

            # Drive the tool through a CMake script rather than a plain COMMAND.
            #
            # The permutation arguments use ffx_sc's `-DNAME={0,1}` syntax. CMake does not
            # escape braces when building the shell command line, and /bin/sh is bash on
            # many systems, where `{0,1}` is brace expansion: the single argument becomes
            # `-DNAME=0 -DNAME=1`, which glslang rejects with "Macro redefined; different
            # substitutions". execute_process passes argv directly with no shell involved,
            # so the braces survive intact.
            set(script "${output_dir}/${name}_compile.cmake")
            set(script_body "execute_process(\n    COMMAND \"${executable}\"\n")
            foreach(argument IN LISTS arguments)
                string(REPLACE "\\" "\\\\" argument "${argument}")
                string(REPLACE "\"" "\\\"" argument "${argument}")
                string(APPEND script_body "        \"${argument}\"\n")
            endforeach()
            string(
                APPEND
                script_body
                "    RESULT_VARIABLE rhine_ffx_result\n    ECHO_OUTPUT_VARIABLE\n    ECHO_ERROR_VARIABLE)\n"
            )
            string(
                APPEND
                script_body
                "if(NOT rhine_ffx_result EQUAL 0)\n    message(FATAL_ERROR \"ffx_shader_compiler failed for ${name} (exit \${rhine_ffx_result})\")\nendif()\n"
            )
            file(GENERATE OUTPUT "${script}" CONTENT "${script_body}")

            add_custom_command(
                OUTPUT "${header}"
                COMMAND "${CMAKE_COMMAND}" -P "${script}"
                WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                DEPENDS "${shader}" "${script}" ${ARGN}
                # ffx_sc -deps=gcc writes <header>.d, so edits to the shared GLSL headers
                # under sdk/include/FidelityFX/gpu retrigger only the affected shaders.
                DEPFILE "${header}.d"
                COMMENT "FidelityFX shader ${name}"
                VERBATIM)
            list(APPEND outputs "${header}")
        endforeach()
    endforeach()

    set(${out_var} "${outputs}" PARENT_SCOPE)
endfunction()
