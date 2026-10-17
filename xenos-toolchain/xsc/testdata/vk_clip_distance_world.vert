#version 450
// A distance from a point other than the position: refused, as Xenos's planes clip the position itself.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; vec4 plane; } pc;

void main()
{
	vec4 world = a_position * pc.scale;
	gl_Position = world + pc.offset;
	gl_ClipDistance[0] = dot(world, pc.plane);
}
