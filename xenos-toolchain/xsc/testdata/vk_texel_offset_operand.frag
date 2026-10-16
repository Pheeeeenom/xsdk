#version 450
// textureOffset: an image operand xsc doesn't take, refused rather than sampled without its offset.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	o_color = textureOffset(u_texture, v_uv, ivec2(1, 0));
}
