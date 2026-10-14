#version 450
// A local scaled, then moved, then read: the position is a_position * scale + offset.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; } pc;

void main()
{
	vec4 p = a_position;
	p = p * pc.scale;
	p = p + pc.offset;
	gl_Position = p;
}
