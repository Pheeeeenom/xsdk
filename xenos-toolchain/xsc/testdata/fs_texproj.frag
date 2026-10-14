#version 450
// textureProj: projective texturing -- divide the coord's xy by its w, then
// sample (shadow maps / projected decals).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_proj;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = textureProj(u_tex, v_proj);
}
