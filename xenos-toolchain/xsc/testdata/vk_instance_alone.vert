#version 450
// gl_InstanceIndex with no block before it: it takes the first register, and the literals the ones after.
layout(location = 0) in vec4 a_position;

void main()
{
	gl_Position = vec4(a_position.x + float(gl_InstanceIndex) * 0.25, a_position.y, 0.0, 1.0);
}
