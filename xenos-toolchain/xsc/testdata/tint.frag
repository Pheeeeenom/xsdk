#version 450
// tint the texture's rgb by the vertex color, keep the texture's own alpha
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    o_color = vec4(tex.rgb * v_color.rgb, tex.a);
}
