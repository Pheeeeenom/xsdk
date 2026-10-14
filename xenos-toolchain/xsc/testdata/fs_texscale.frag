#version 450
// computed texture coordinate (not a bare interpolator): uv*2 + offset, then
// sample -- exercises the generalized coord path.
layout(location = 0) in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = texture(u_tex, v_uv * 2.0 + vec2(0.1, 0.2));
}
