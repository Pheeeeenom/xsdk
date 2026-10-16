#version 450
// gl_FrontFacing in a ?:, glslang's select: the interpolated colour on a front face, green on a back one.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = gl_FrontFacing ? v_color : vec4(0.0, 1.0, 0.0, 1.0);
}
