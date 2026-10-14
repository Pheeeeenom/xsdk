#version 450
// float division: lowered as per-component reciprocal-multiply (a * rcp(b)).
// Divides the sampled texel by a per-channel denominator uniform. Exercises
// SPIR-V OpFDiv (133-adjacent, opcode 136) which xsc did not handle.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform vec4 u_denom;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    o_color = tex / u_denom;
}
