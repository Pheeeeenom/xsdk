#version 450
// gl_ClipDistance as Xenos's user clip planes take it, dot(gl_Position, a plane from a block): one plane from push
// constants and two from a uniform block's array, the position read back from gl_Position and through a local.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; vec4 plane; } pc;
layout(set = 0, binding = 0) uniform Planes { vec4 planes[2]; } u;

void main()
{
	vec4 p = a_position * pc.scale + pc.offset;
	gl_Position = p;
	gl_ClipDistance[0] = dot(gl_Position, pc.plane);
	gl_ClipDistance[2] = dot(u.planes[1], p);
	gl_ClipDistance[5] = dot(p, u.planes[0]);
}
