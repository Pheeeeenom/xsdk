#version 450
// Two colour outputs reading a local an if/else assigns: c is a where uv.x > 0.5, else b; o_color0 c, o_color1 c * 2.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color0;
layout(location = 1) out vec4 o_color1;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	vec4 c;
	if (v_uv.x > 0.5) {
		c = pc.a;
	} else {
		c = pc.b;
	}
	o_color0 = c;
	o_color1 = c * 2.0;
}
