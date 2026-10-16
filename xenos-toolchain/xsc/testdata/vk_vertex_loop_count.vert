#version 450
// A vertex shader's loop a push constant counts, moving the position a step a pass and summing a colour
// (tests/test_hardware_loops.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 step; int count; } pc;

void main()
{
	vec4 p = a_position;
	vec4 c = vec4(0.0);
	for (int i = 0; i < pc.count; i++) {
		p.xy += pc.step.xy;
		c += a_color * 0.25;
	}
	gl_Position = p;
	v_color = c;
}
