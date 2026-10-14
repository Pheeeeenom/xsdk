#version 450
// Four samples a texel apart, at coordinates the shader works out: the SDK's texel-rate probe's shader
// (tools/gen_texel_shaders.py), which xsc lays out with the coordinates first and the fetches together.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { vec4 delta; vec4 scale; } pc;

void main()
{
	o_color = (texture(u_texture, v_uv) + texture(u_texture, v_uv + pc.delta.xz) +
		texture(u_texture, v_uv + pc.delta.zy) + texture(u_texture, v_uv + pc.delta.xy)) * pc.scale;
}
