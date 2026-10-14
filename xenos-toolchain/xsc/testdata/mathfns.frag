#version 450
// transcendental / geometric builtins: normalize a varying direction, take
// its length, raise the texture rgb to a uniform gamma, and floor the uv.
// Exercises GLSL.std.450 Normalize, Length, Pow, Floor — all unhandled.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform float u_gamma;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    vec3 dir = normalize(v_color.rgb);
    float len = length(v_color.rgb);
    vec3 g = pow(tex.rgb, vec3(u_gamma));
    vec2 fl = floor(v_uv);
    o_color = vec4(dir * len + g, fl.x);
}
