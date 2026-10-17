#version 450
// A point as wide as its push constants say: gl_PointSize goes to oPts.x, the point's width in pixels.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 offset; float size; } pc;

void main()
{
	gl_Position = a_position + pc.offset;
	gl_PointSize = pc.size;
}
