#version 450
// An if/else on a push constant inside an unrolled loop's body: each pass's arms in execs on the predicate
// (tests/test_loop_branches.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; float mode; } pc;

void main()
{
	vec4 sum = vec4(v_uv, 0.0, 1.0);
	for (int i = 0; i < 3; i++) {
		if (pc.mode > 0.5)
			sum = sum * 0.5 + pc.step;
		else
			sum.z += 0.25;
	}
	o_color = sum;
}
