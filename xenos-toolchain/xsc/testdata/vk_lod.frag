#version 450
// textureLod at a LOD the shader works out, at one the fetch's own bias field holds and at one past it, and texture()
// with a bias the shader works out: each sample's LOD lands in one component.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	o_color = vec4(textureLod(u_texture, v_uv, v_uv.x * 8.0).x, textureLod(u_texture, v_uv, 2.0).y,
		textureLod(u_texture, v_uv, 5.0).z, texture(u_texture, v_uv, v_uv.y * 12.0).w);
}
