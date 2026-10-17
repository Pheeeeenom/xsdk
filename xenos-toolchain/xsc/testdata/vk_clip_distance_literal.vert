#version 450
// A plane written in the shader rather than read from a block: refused, as the runtime sets the planes.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; } pc;

void main()
{
	gl_Position = a_position * pc.scale + pc.offset;
	gl_ClipDistance[0] = dot(gl_Position, vec4(1.0, 0.0, 0.0, 0.5));
}
