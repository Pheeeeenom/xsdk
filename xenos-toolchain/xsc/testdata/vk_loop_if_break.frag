#version 450
// A break on data out of an unrolled loop, refused: the passes after it would run for some pixels and not others
// (tests/test_loop_branches.py).
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	for (int i = 0; i < 4; i++) {
		sum += pc.step;
		if (sum.x > v_color.x)
			break;
	}
	o_color = sum;
}
