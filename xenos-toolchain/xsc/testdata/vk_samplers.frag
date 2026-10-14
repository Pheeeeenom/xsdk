#version 450
// Samplers by set and binding, and push constants from an offset.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 1, binding = 0) uniform sampler2D u_texture;
layout(set = 0, binding = 2) uniform sampler2D u_other;
layout(push_constant) uniform Push { layout(offset = 16) vec4 tint; } pc;

void main()
{
    o_color = texture(u_texture, v_uv) * pc.tint + texture(u_other, v_uv);
}
