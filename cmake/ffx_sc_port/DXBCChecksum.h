// Rhine port shim: DXBC checksums only matter on the HLSL/DXBC path, which is not
// built here (see hlsl_compiler.h). utils.h includes this header, so it must exist.
#pragma once
