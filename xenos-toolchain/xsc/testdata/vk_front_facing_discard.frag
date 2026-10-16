#version 450
// A back face discarded, as one-sided foliage is.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
	if (!gl_FrontFacing)
		discard;
	o_color = v_color;
}
