// Rhine port shim — replaces the SDK's hlsl_compiler.h for non-Windows builds.
//
// ffx_sc.cpp only ever names HLSLCompiler::Backend and its constructor (the GLSL path is
// selected with `-compiler=glslang`), so this stub keeps that surface and drops the DXC
// COM members. Any attempt to actually compile HLSL throws — the Vulkan backend never
// asks for it, and DX12 shaders are generated on Windows where the real tool is built.

#pragma once

#include "compiler.h"

/// Stub of the HLSL specialization of <c><i>ICompiler</i></c>.
/// Present only so ffx_sc.cpp can select a compiler; the implementation throws.
class HLSLCompiler : public ICompiler
{
public:
    /// Mirrors the upstream backend enumeration so ffx_sc.cpp keeps compiling.
    enum Backend
    {
        DXC,
        GDK_SCARLETT_X64,
        GDK_XBOXONE_X64,
        FXC
    };

public:
    HLSLCompiler(Backend            backend,
                 const std::string& dll,
                 const std::string& shaderPath,
                 const std::string& shaderName,
                 const std::string& shaderFileName,
                 const std::string& outputPath,
                 bool               disableLogs,
                 bool               debugCompile);

    ~HLSLCompiler();

    bool Compile(Permutation&                    permutation,
                 const std::vector<std::string>& arguments,
                 std::mutex&                     writeMutex) override;

    bool ExtractReflectionData(Permutation& permutation) override;

    void WriteBinaryHeaderReflectionData(FILE* fp, const Permutation& permutation, std::mutex& writeMutex) override;

    void WritePermutationHeaderReflectionStructMembers(FILE* fp) override;

    void WritePermutationHeaderReflectionData(FILE* fp, const Permutation& permutation) override;
};
