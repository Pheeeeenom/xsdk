#version 450
// An if/else on data inside an unrolled loop's body (tests/test_vertex_branches.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	for (int i = 0; i < 4; i++) {
		if (v_uv.x > float(i) * 0.25)
			sum += pc.step;
		else
			sum.w += 0.125;
	}
	o_color = sum;
}
