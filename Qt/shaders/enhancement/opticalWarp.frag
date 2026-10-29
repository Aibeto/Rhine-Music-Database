#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"

// 相位 0.5：两帧正中间（与 FFX 插值相位一致，便于对比）
#define WARP_T 0.5
#include "opticalWarpBody.glsl"
