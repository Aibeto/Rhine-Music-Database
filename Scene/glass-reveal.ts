// 柔化的透明前沿从封面顶部向底部移动。
// 进度两端分别完全结霜和完全透明，包含边缘。
const FEATHER = 0.12;
export const FROSTED_ROUGHNESS = 0.42;
export const CLEAR_ROUGHNESS = 0.025;
const FROST_SPAN = 0.016;

// Three 默认的 mip 规则会在窗口缩小时放大相对模糊范围；
// 将磨砂覆盖范围限制在投影封面高度的 1.6%。
export function frostedTransmissionLod(
    panelPixels: number,
    textureWidth: number,
    roughness: number,
    quality = 1,
    ior = 1.46,
) {
    const clamp = (v: number) => Math.max(0, Math.min(1, v));
    const strength = clamp((roughness - CLEAR_ROUGHNESS) / (FROSTED_ROUGHNESS - CLEAR_ROUGHNESS));
    const native = Math.log2(textureWidth) * roughness * clamp(ior * 2 - 2);
    const clearLod = Math.log2(textureWidth) * CLEAR_ROUGHNESS * clamp(ior * 2 - 2);
    const bounded = Math.log2(Math.max(2 ** clearLod, panelPixels * FROST_SPAN * Math.pow(strength, 1.15)));
    return native + (Math.min(native, bounded) - native) * quality;
}
export const frostedTransmissionGLSL = `
float archiveTransmissionLod(float roughness, float ior, vec2 samplerSize) {
  float nativeLod = log2(samplerSize.x) * roughness * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
  float strength = clamp((roughness - ${CLEAR_ROUGHNESS}) / ${FROSTED_ROUGHNESS - CLEAR_ROUGHNESS}, 0.0, 1.0);
  float panelPixels = length(vArchiveProjectedAxis * samplerSize);
  float clearLod = log2(samplerSize.x) * ${CLEAR_ROUGHNESS} * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
  float boundedLod = log2(max(exp2(clearLod), panelPixels * ${FROST_SPAN} * pow(strength, 1.15)));
  return mix(nativeLod, min(nativeLod, boundedLod), archiveQuality);
}`;
export function glassRevealAtHeight(progress: number, height: number) {
    const edge = 1 - (1 + 2 * FEATHER) * Math.max(0, Math.min(1, progress));
    const x = Math.max(0, Math.min(1, (Math.max(0, Math.min(1, height)) - edge) / (2 * FEATHER)));
    return x * x * (3 - 2 * x);
}
export const glassRevealGLSL = `
float glassRevealAtHeight(float progress, float height) {
  float edge = 1.0 - ${1 + 2 * FEATHER} * clamp(progress, 0.0, 1.0);
  return smoothstep(edge, edge + ${2 * FEATHER}, clamp(height, 0.0, 1.0));
}`;
