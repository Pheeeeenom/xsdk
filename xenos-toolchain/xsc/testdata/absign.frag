#version 450
// abs / sign / min / max on a signed uniform: fold a bipolar offset uniform
// into the texture. abs() gives magnitude, sign() gives direction, and
// min/max clamp against the vertex color. Exercises GLSL.std.450 FAbs and
// FSign (both unhandled) alongside FMin/FMax (already handled).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform vec4 u_offset;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    vec4 mag = abs(u_offset);
    vec4 dir = sign(u_offset);
    vec4 lo = min(tex, v_color);
    vec4 hi = max(tex + mag * dir, lo);
    o_color = hi;
}
