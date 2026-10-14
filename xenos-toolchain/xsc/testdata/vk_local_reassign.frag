#version 450
// A local stored twice, the first value kept in another first: o_color is a * x + b * y.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	vec4 t = pc.a * v_uv.x;
	vec4 first = t;
	t = pc.b * v_uv.y;
	o_color = first + t;
}
