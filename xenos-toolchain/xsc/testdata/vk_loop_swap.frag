#version 450
// A loop a push constant counts swapping two locals a pass, which its registers take all at once
// (tests/test_hardware_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; int count; } pc;

void main()
{
	vec4 x = pc.a;
	vec4 y = pc.b;
	for (int i = 0; i < pc.count; i++) {
		vec4 t = x;
		x = y;
		y = t + vec4(0.125);
	}
	o_color = x * 0.5 + y;
}
