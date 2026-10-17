#version 450
// A cull distance: refused, as Xenos has none.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; vec4 plane; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
	gl_CullDistance[0] = dot(gl_Position, pc.plane);
}
