#version 450
// A local stored twice beside an if/else, which the unroller can't rewrite: refused.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	vec4 t = pc.a * v_uv.x;
	vec4 first = t;
	t = pc.b;
	vec4 c;
	if (v_uv.y > 0.5)
		c = first;
	else
		c = t;
	o_color = c;
}
