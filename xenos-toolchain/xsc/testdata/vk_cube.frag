#version 450
// A cube sampled in the direction the vertex shader passes on.
layout(location = 0) in vec4 v_coord;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform samplerCube u_cube;

void main()
{
	o_color = texture(u_cube, v_coord.xyz);
}
