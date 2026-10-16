#version 450
// A position, and a coordinate of three components passed on for xsdk_test's cube and 3D textures.
layout(location = 0) in vec4 a_position;
layout(location = 2) in vec4 a_coord;
layout(location = 0) out vec4 v_coord;

void main()
{
	gl_Position = a_position;
	v_coord = a_coord;
}
