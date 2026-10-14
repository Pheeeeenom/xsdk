#version 450
// arbitrary-bounds clamp (NOT [0,1]): keep the modulated color inside a
// fixed mid-range window so it never goes fully black or fully white.
// Exercises GLSL.std.450 FClamp with bounds (0.2, 0.8) — xsc only handles
// the (0.0, 1.0) saturate special case today.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    o_color = clamp(tex * v_color, 0.2, 0.8);
}
