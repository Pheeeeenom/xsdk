#version 450
// textureLod: explicit LOD (mip level) folded into the tfetch LOD-bias field.
layout(location = 0) in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = textureLod(u_tex, v_uv, 2.0);
}
