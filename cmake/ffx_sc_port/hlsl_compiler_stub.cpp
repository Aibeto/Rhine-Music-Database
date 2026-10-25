// Rhine port shim — HLSL compiler stub.
//
// The Vulkan backend drives ffx_shader_compiler with `-compiler=glslang`, so the HLSL
// path is unreachable in this build. Failing loudly beats silently producing DXIL that
// cannot be consumed here; DX12 shaders are generated on Windows with the real tool.

#include "hlsl_compiler.h"

#include <stdexcept>

namespace {

[[noreturn]] void unsupported()
{
    throw std::runtime_error(
        "HLSL shader compilation is unavailable in this build; "
        "use -compiler=glslang for the Vulkan backend");
}

}  // namespace

HLSLCompiler::HLSLCompiler(Backend /*backend*/, const std::string& /*dll*/, const std::string& shaderPath,
                           const std::string& shaderName, const std::string& shaderFileName,
                           const std::string& outputPath, bool disableLogs, bool debugCompile)
    : ICompiler(shaderPath, shaderName, shaderFileName, outputPath, disableLogs, debugCompile)
{
}

HLSLCompiler::~HLSLCompiler() = default;

bool HLSLCompiler::Compile(Permutation& /*permutation*/, const std::vector<std::string>& /*arguments*/,
                           std::mutex& /*writeMutex*/)
{
    unsupported();
}

bool HLSLCompiler::ExtractReflectionData(Permutation& /*permutation*/)
{
    unsupported();
}

void HLSLCompiler::WriteBinaryHeaderReflectionData(FILE* /*fp*/, const Permutation& /*permutation*/,
                                                   std::mutex& /*writeMutex*/)
{
    unsupported();
}

void HLSLCompiler::WritePermutationHeaderReflectionStructMembers(FILE* /*fp*/)
{
    unsupported();
}

void HLSLCompiler::WritePermutationHeaderReflectionData(FILE* /*fp*/, const Permutation& /*permutation*/)
{
    unsupported();
}
