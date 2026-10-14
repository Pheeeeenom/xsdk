#version 450
// The if/else's variable read before the if/else: refused, as the lowering would read the merged value there.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	vec4 c = pc.a;
	vec4 early = c * v_uv.x;
	if (v_uv.y > 0.5)
		c = pc.b;
	else
		c = pc.a * v_uv.y;
	o_color = c + early;
}
