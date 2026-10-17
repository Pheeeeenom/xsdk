#version 450
// A point sprite's coordinates as its colour: gl_PointCoord's x in red and y in green.
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(gl_PointCoord, 0.0, 1.0);
}
