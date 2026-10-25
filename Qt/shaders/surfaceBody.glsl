layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in float v_height;
layout(location = 4) in float v_viewZ;
layout(location = 5) in float v_quality;
layout(location = 6) in float v_clarity;
layout(location = 7) in float v_theme;
layout(location = 8) in vec4 v_shadow;
Out interpolated() {
    return Out(gl_FragCoord, v_world, v_normal, v_uv, v_height, v_viewZ, v_quality, v_clarity, v_theme,
               v_shadow);
}
layout(std140, binding = 1) uniform SurfaceBlock {
    Surface m;
};
layout(binding = 2) uniform sampler2D env;
layout(binding = 3) uniform sampler2D capture;
layout(binding = 4) uniform sampler2DShadow shadow;
layout(binding = 5) uniform sampler2D label;
layout(binding = 6) uniform sampler2D lut;
layout(binding = 7) uniform sampler2D cover;
layout(binding = 8) uniform sampler2D captureHalf;
layout(binding = 9) uniform sampler2D captureQuarter;
layout(binding = 10) uniform sampler2D captureEighth;
layout(location = 0) out vec4 fragColor;
/**
 * 根据世界空间法线和观察方向计算表面光照。
 * 保留原磨砂、透射、金属、涂层及雾效的合成顺序。
 */
vec4 surface(Out o, bool front) {
    vec3 N = normalize(o.normal);
    if (!front && m.flags.y > 1.5) {
        N = -N;
    }
    vec3 V = normalize(g.eye.xyz - o.world);
    float nv = saturate(dot(N, V));
    vec3 base = m.color.rgb;
    float r = m.physical.x;
    float metal = m.physical.y;
    float trans = m.physical.z;
    float thick = m.physical.w;
    float clear = o.clarity;
    float alpha = m.color.a;
    if (m.flags.x == 1) {
        // 标签按图集槽位采样：atlas = {offsetX, offsetY, scaleX, scaleY}，
        // 非图集采样的部件（装饰印记、装饰文字）为全图 {0,0,1,1}。
        vec2 luv = vec2(o.uv.x, 1 - o.uv.y) * m.atlas.zw + m.atlas.xy;
        vec4 v = texture(label, luv);
        vec3 rgb = linearColor(v.rgb);
        float amount = 1 - smoothstep(.12, .65, dot(rgb, vec3(.2126, .7152, .0722)));
        rgb = mix(rgb, mix(vec3(.023, .032, .037), vec3(.78, .78, .71), amount), o.theme);
        return vec4(rgb, v.a * o.quality);
    }
    if (m.flags.x == 7) {
        // 卡片上独立的装饰文字：只做线性化后直接叠加，不参与光照、雾与主题混合。
        vec4 v = texture(label, vec2(o.uv.x, 1 - o.uv.y));
        return vec4(linearColor(v.rgb), v.a);
    }
    if (m.flags.x == 6) {
        // 只旋转盘面采样坐标；圆环几何、外壳、法线和缓存阴影保持不动。
        vec2 discUV = vec2(o.uv.x, 1 - o.uv.y);
        if (m.extra.z != 1 || m.extra.w != 0) {
            vec2 centered = discUV - vec2(.5);
            discUV = mat2(m.extra.z, -m.extra.w, m.extra.w, m.extra.z) * centered + vec2(.5);
        }
        base = linearColor(texture(cover, discUV).rgb);
    }
#ifdef RHINE_HASHED
    // 卡片淡入的抖动透明。只有 surfaceHashed.frag 定义 RHINE_HASHED：只要 shader 里
    // 存在 discard，硬件就会对整个管线禁用 early-Z，所以主不透明管线不能带这个分支。
    if (m.flags.x == 2.0) {
        float coverage =
            fract(52.9829189 *
                  fract(dot(vec2(o.position.x, backend.x > .5 ? o.position.y : g.viewport.y - o.position.y),
                            vec2(.06711056, .00583715))));
        if (o.quality <= coverage) {
            discard;
        }
    }
#endif
    if (m.flags.x == 3) {
        base *= mix(mix(vec3(.40, .30, .20), vec3(1, .98, .94), smoothstep(.1, 1.0, o.height)), vec3(1),
                    o.quality);
        float edge = 1 - 1.24 * saturate(clear);
        float reveal = smoothstep(edge, edge + .24, saturate(o.height));
        r = mix(mix(.28, .13, o.quality), .055, reveal);
    }
    vec3 dark = m.darkColor.xyz;
    if (m.flags.x == 3 && m.darkColor.w < .5) {
        float edge = 1 - 1.24 * saturate(clear);
        dark = mix(dark, vec3(.92, .96, .97), smoothstep(edge, edge + .24, saturate(o.height)));
    }
    if (m.optics.z > .5) {
        dark = mix(dark, vec3(.030, .042, .048), m.flags.z);
    }
    if (m.flags.w < .5 && m.flags.x != 6) {
        base = mix(base, dark, o.theme);
    }

    vec3 dxy = max(abs(dFdx(N)), abs(dFdy(N)));
    float geomRough = max(max(dxy.x, dxy.y), dxy.z);
    r = min(1.0, max(r, .0525) + geomRough);
    vec3 f0 = mix(vec3(pow((m.optics.x - 1) / (m.optics.x + 1), 2)), base, metal);
    vec3 diff = base * (1 - metal);
    vec3 L = normalize(vec3(-6, 14, -5));
    vec3 F = normalize(vec3(7, 8, -10));
    vec3 key = linearColor(vec3(1, .969, .929)) * 1.4 * (1 - .35 * o.theme);
    vec3 fill = vec3(.6) * (1 - .35 * o.theme);
    vec3 directD =
        diff / PI * (key * max(dot(N, L), 0.0) * shadowFactor(o, shadow, g) + fill * max(dot(N, F), 0.0));
    vec3 directS = key * max(dot(N, L), 0.0) * ggxMulti(N, V, L, f0, r, lut) * shadowFactor(o, shadow, g) +
                   fill * max(dot(N, F), 0.0) * ggxMulti(N, V, F, f0, r, lut);
    float intensity = mix(.48, .32, o.theme);
    vec3 hemi = mix(linearColor(vec3(.706, .631, .549)), linearColor(vec3(1, .980, .961)),
                    dot(N, vec3(0, 1, 0)) * .5 + .5) *
                .65 * (1 - .35 * o.theme);
    vec3 irr = envSample(env, N, 1.0) * PI * intensity;
    vec3 R = normalize(mix(reflect(-V, N), N, r * r * r * r));
    vec3 radiance = envSample(env, R, r) * intensity;
    vec2 fab = DFG(nv, r, lut);
    vec3 dielectric = vec3(pow((m.optics.x - 1) / (m.optics.x + 1), 2));
    vec3 single = mix(dielectric * fab.x + fab.y, base * fab.x + fab.y, metal);
    vec3 multi = mix(multiScatter(dielectric, fab), multiScatter(base, fab), metal);
    vec3 indirectS = radiance * single + irr / PI * multi;
    vec3 indirectD = diff * (1 - (dielectric * fab.x + fab.y + multiScatter(dielectric, fab))) * irr / PI +
                     diff * hemi / PI;
    vec3 diffuse = directD + indirectD;
    if (trans > .000001) {
        vec3 ray = normalize(refract(-V, N, 1 / m.optics.x)) * thick;
        vec4 pos = g.projection * g.view * vec4(o.world + ray, 1);
        vec2 uv = pos.xy / pos.w * .5 + .5;
        uv.y = 1 - uv.y;
        // 斜视角下折射光线的投影偏移会被视角放大，封面色块可能被拉到屏幕另一端成破图。
        // 限幅必须与翻转在同一空间：selfUv 走同一翻转后再比较，否则方向镜像。
        vec4 selfPos = g.projection * g.view * vec4(o.world, 1);
        vec2 selfUv = selfPos.xy / selfPos.w * .5 + .5;
        selfUv.y = 1 - selfUv.y;
        // 掠射角（nv→0）时折射光在玻璃内全反射，大偏移采样在物理与视觉上都不成立——
        // 镜像错位只在斜视角出现。偏移幅度随 nv 收缩：正面全偏移，掠射归零。
        uv = mix(selfUv, clamp(uv, selfUv - .07, selfUv + .07), saturate(nv * 2.0));
        float nativeLod = log2(float(textureSize(capture, 0).x)) * r * clamp(m.optics.x * 2 - 2, 0.0, 1.0);
        float projected = 3.7 * g.projection[1][1] / max(o.viewZ, .001) * g.viewport.y * .5;
        float strength = saturate((r - .025) / .395);
        float clearLod = log2(float(textureSize(capture, 0).x)) * .025 * clamp(m.optics.x * 2 - 2, 0.0, 1.0);
        float bounded = log2(max(exp2(clearLod), projected * .016 * pow(strength, 1.15)));
        float lod = m.flags.x == 3 ? mix(nativeLod, min(nativeLod, bounded), o.quality) : nativeLod;
        // 所有后端统一采样抓屏纹理。曾按后端把非 GL 后端切到环境图采样，结果玻璃后的
        // 唱片完全消失（除列表与列表→详情动画外都看不到 CD 内部），且 env 图集在部分
        // 方向会采到黑块，形成"折射半截黑边"。抓屏已改为 RGBA8 单层且不生成 mip，
        // 不再有"纹理不完整导致采样为黑"的隐患。
        // 详情封面使用原始采样
        vec3 transmissionColor = m.optics.w > .5 ? textureLod(capture, rtUV(uv), 0).rgb
                                                 : transmissionSample(capture, captureHalf, captureQuarter, captureEighth, uv, lod).rgb;
        vec3 atten = exp(log(max(m.attenuation.xyz, vec3(.00001))) * length(ray) / max(m.optics.y, .0001));
        vec3 refracted = (1 - envBRDF(f0, nv, r, lut)) * base * atten * transmissionColor;
        // sharpCover 的材质只保留极弱的折射：唱片本身已经直接可见，折射会把同一张封面
        // 再采样一次并偏移成像，旋转时就看成一圈拖影。
        diffuse = mix(diffuse, refracted, m.optics.w > .5 ? trans * .12 : trans);
    }
    vec3 color = diffuse + directS + indirectS;
    if (m.extra.x > 0) {
        float cc = m.extra.x;
        float cr = min(1.0, max(m.extra.y, .0525) + geomRough);
        float f = .04 + .96 * pow(1 - nv, 5);
        vec3 coat = key * max(dot(N, L), 0.0) * ggx(N, V, L, vec3(.04), cr) +
                    fill * max(dot(N, F), 0.0) * ggx(N, V, F, vec3(.04), cr) +
                    envSample(env, reflect(-V, N), cr) * intensity * envBRDF(vec3(.04), nv, cr, lut);
        color = color * (1 - cc * f) + coat * cc;
    }
    if (m.flags.x != 6 && m.extra.z > 0) {
        alpha = min(.86, alpha + .42 * pow(1 - abs(dot(N, V)), 3.0));
    }
    color = mix(color, g.fogColor.xyz, smoothstep(g.fog.x, g.fog.y, o.viewZ));
    return vec4(color, alpha);
}
void main() {
    fragColor = surface(interpolated(), gl_FrontFacing);
}
