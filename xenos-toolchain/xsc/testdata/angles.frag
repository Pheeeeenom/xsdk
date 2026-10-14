#version 450
// radians()/degrees(): a single multiply by a pooled constant (pi/180,
// 180/pi). Trivial but common in procedural shaders.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(u_tex, v_uv);
    o_color = radians(v_color) + degrees(t);
}
