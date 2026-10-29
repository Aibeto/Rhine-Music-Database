#version 440
#extension GL_GOOGLE_include_directive : enable
#include "opticalCommon.glsl"
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 model0;
layout(location = 4) in vec4 model1;
layout(location = 5) in vec4 model2;
layout(location = 6) in vec4 model3;
layout(location = 7) in vec4 state;
#ifdef RHINE_SINGLE_PASS_MOTION
layout(location = 8) in vec4 previousModel0;
layout(location = 9) in vec4 previousModel1;
layout(location = 10) in vec4 previousModel2;
layout(location = 11) in vec4 previousModel3;
#endif
layout(location = 0) out vec2 v_surfaceUv;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_lightUv;
layout(location = 3) out float v_theme;
layout(location = 4) out float v_viewZ;
layout(location = 5) out float v_quality;
layout(location = 6) out float v_lightMask;
layout(location = 7) out vec2 v_normalUv;
layout(location = 8) out float v_clarity;
layout(location = 9) out vec3 v_world;
layout(location = 10) out vec3 v_normal;
layout(location = 11) out float v_mode;
#ifdef RHINE_SINGLE_PASS_MOTION
// 运动矢量与颜色同一趟输出：省掉整趟几何的第二次栅格化。
layout(location = 12) out vec2 v_velocity;
#endif
void main() {
    mat4 model = mat4(model0, model1, model2, model3);
    vec4 world = model * vec4(position, 1);
    vec4 eye = g.view * world;
    gl_Position = correction * g.projection * eye;
#ifdef RHINE_SINGLE_PASS_MOTION
    // 速度用未抖动的当前/上一帧 VP，避免把采样抖动误当几何运动（与 motion.vert 同口径）。
    mat4 previousModel = mat4(previousModel0, previousModel1, previousModel2, previousModel3);
    vec4 currentClip = correction * currentViewProjection * world;
    vec4 previousClip = correction * previousViewProjection * previousModel * vec4(position, 1);
    vec2 ndc = currentClip.xy / currentClip.w;
    vec2 previousNdc = previousClip.xy / previousClip.w;
    v_velocity = vec2(previousNdc.x - ndc.x, ndc.y - previousNdc.y) * 0.5;
#endif
    float face = getFace(normal);
    vec2 surfaceUv = vec2((position.x + 2.5) / 5.0, position.y / 3.7);
    if (face == 0.0 || face == 3.0) {
        surfaceUv.x = (position.z + 0.25) / 0.5;
    } else if (face == 1.0 || face == 4.0) {
        surfaceUv.y = (position.z + 0.25) / 0.5;
    }
    // 每块图集留半个纹素边界，避免相邻朝向与昼夜行发生串色。
    surfaceUv = (saturate(surfaceUv) * 383.0 + 0.5) / 384.0;
    v_surfaceUv = vec2((face + surfaceUv.x) / 6.0, surfaceUv.y / 2.0);
    vec3 local = (emitterInverse * world).xyz;
    vec3 localNormal = normalize((emitterInverse * model * vec4(normal, 0)).xyz);
    float lightFace = getFace(localNormal);
    vec2 fieldUv = saturate(vec2((local.x + 9.0) / 18.0, (local.z + 7.0) / 14.0));
    fieldUv = (fieldUv * 127.0 + 0.5) / 128.0;
    v_lightUv = vec3((lightFace + fieldUv.x) / 6.0, fieldUv.y, saturate((local.y + 0.4) / 5.2) * 7.0);
    v_lightMask = step(abs(local.x), 9.0) * step(abs(local.z), 7.0);
    v_uv = uv;
    v_theme = state.z;
    v_viewZ = -eye.z;
    v_quality = state.x;
    v_clarity = state.y;
    v_world = world.xyz;
    v_normal = normalize((model * vec4(normal, 0)).xyz);
    v_mode = state.w;
    vec3 direction = normalize(normal);
    vec2 encoded = direction.xy / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    if (direction.z < 0.0) {
        encoded = (1.0 - abs(encoded.yx)) * step(vec2(0.0), encoded) * 2.0 - (1.0 - abs(encoded.yx));
    }
    v_normalUv = ((encoded * 0.5 + 0.5) * 127.0 + 0.5) / 128.0;
    v_normalUv.y *= 0.5;
}
