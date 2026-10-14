#version 450
// non-identity swizzle: read the texture's channels in BGRA order, then
// modulate by the vertex color. Exercises a VectorShuffle whose index list
// is NOT an identity prefix (channel reorder), which xsc cannot lower yet.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    o_color = tex.bgra * v_color;
}
