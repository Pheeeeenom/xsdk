#version 450
// A loop a push constant counts, adding a step a pass, as a hardware loop whose count the runtime sets from the block
// (tests/test_hardware_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 step; vec4 base; int count; } pc;

void main()
{
	vec4 sum = pc.base;
	for (int i = 0; i < pc.count; i++)
		sum += pc.step;
	o_color = sum;
}
