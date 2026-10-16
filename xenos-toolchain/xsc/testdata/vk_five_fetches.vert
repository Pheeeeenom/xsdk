#version 450
// Five vertex inputs, so five fetches in one exec, as Aurora's GX vertices with two texture coordinates, a normal and
// a matrix index have.
layout(location = 0) in vec3 in_pos;
layout(location = 2) in vec2 in_tex0;
layout(location = 3) in vec2 in_tex1;
layout(location = 4) in vec3 in_nrm;
layout(location = 5) in float in_index;
layout(location = 0) out vec4 out_sum;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
	out_sum = vec4(in_tex0 + in_tex1, in_index, 0.0) + vec4(in_nrm, 1.0);
}
