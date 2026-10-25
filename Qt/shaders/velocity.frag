#version 440

// RG = 屏幕 UV 位移（current → previous，y 向下），B/A 预留给后续 FSR3 扩展。
layout(location = 0) in vec2 v_velocity;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(v_velocity, 0.0, 1.0);
}
