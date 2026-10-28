#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"

// 相位 0.75：4 倍插帧的第二张中间帧
#define WARP_T 0.75
#include "opticalWarpBody.glsl"
