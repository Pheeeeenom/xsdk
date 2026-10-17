#version 450
// A 3D texture sampled at the coordinate the vertex shader passes on.
layout(location = 0) in vec4 v_coord;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler3D u_volume;

void main()
{
	o_color = texture(u_volume, v_coord.xyz);
}
