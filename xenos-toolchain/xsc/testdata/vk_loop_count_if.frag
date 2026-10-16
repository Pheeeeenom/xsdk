#version 450
// A loop a uniform buffer counts, with an if/else on data in its pass, the pass's index in a block's array and as a
// float, and a break on what the pass carries (tests/test_hardware_loops.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Loop { vec4 colors[8]; vec4 limit; int count; } u;

void main()
{
	vec4 sum = vec4(0.0);
	vec4 seen = vec4(0.0);
	for (int i = 0; i < u.count; i++) {
		if (v_uv.x > float(i) * 0.125)
			seen += u.colors[i];
		else
			seen.w += 0.0625;
		sum += u.colors[i] * 0.5;
		if (sum.y >= u.limit.y)
			break;
	}
	o_color = seen + sum * 0.25;
}
