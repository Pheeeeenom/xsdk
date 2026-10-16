#version 450
// A loop a push constant counts breaking on data, refused: LOOP_END breaks when every pixel of 64 agrees
// (tests/test_hardware_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 sum = vec4(0.0);
	for (int i = 0; i < pc.count; i++) {
		sum += pc.step;
		if (sum.x > v_uv.x)
			break;
	}
	o_color = sum;
}
