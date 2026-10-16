#version 450
// textureGrad with a horizontal gradient from a push constant and a vertical one the shader builds.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { vec4 gradient; } pc;

void main()
{
	o_color = textureGrad(u_texture, v_uv, pc.gradient.xy, vec2(0.0, v_uv.y * 0.5));
}
