#version 450
// A store into one component of a local: refused.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; } pc;

void main()
{
	vec4 t = pc.a;
	t.x = v_uv.x;
	o_color = t;
}
