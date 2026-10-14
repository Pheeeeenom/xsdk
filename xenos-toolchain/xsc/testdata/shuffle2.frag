#version 450
// two-source VectorShuffle: assemble a vec4 from components of two different
// vectors (texel.xy + color.zw). Each output component routes from one of the
// two sources through a masked mov.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(u_tex, v_uv);
    o_color = vec4(t.xy, v_color.zw);
}
