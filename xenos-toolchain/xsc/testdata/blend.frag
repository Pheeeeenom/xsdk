#version 450
// a novel shader xsc has never seen: per-channel blend between a texture and
// the vertex color, controlled by a uniform vec4 (dashboard fade transitions)
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform vec4 u_blend;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    o_color = mix(tex, v_color, u_blend);
}
