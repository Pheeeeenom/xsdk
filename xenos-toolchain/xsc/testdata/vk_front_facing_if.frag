#version 450
// gl_FrontFacing as an if's condition, and gl_FragCoord beside it from the same pixel parameters.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	if (gl_FrontFacing)
		o_color = v_color;
	else
		o_color = vec4(gl_FragCoord.xy * 0.001, 0.0, 1.0);
}
