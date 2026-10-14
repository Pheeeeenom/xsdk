#version 450
// refract(): Snell's law refraction vector (GLSL.std.450 #72).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    vec3 I = normalize(vec3(v_uv * 2.0 - 1.0, 1.0));
    vec3 N = vec3(0.0, 0.0, 1.0);
    o_color = vec4(refract(I, N, 0.66) * 0.5 + 0.5, 1.0);
}
