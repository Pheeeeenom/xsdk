#version 450
// Six vertex inputs, so six fetches in one exec, as Aurora's GX vertices with two texture coordinates, a binormal and
// a tangent have; vk_five_fetches.vert drops the tangent.
layout(location = 0) in vec3 in_pos;
layout(location = 2) in vec2 in_tex0;
layout(location = 3) in vec2 in_tex1;
layout(location = 4) in vec3 in_nrm;
layout(location = 5) in vec3 in_binrm;
layout(location = 6) in vec3 in_tangent;
layout(location = 0) out vec4 out_sum;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
	out_sum = vec4(in_tex0 + in_tex1, 0.0, 0.0) + vec4(in_nrm + in_binrm + in_tangent, 1.0);
}
