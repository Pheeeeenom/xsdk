#version 450
// A second colour output assigned in an if/else, which the lowering can't take beside another output.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color0;
layout(location = 1) out vec4 o_color1;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	o_color0 = pc.a;
	if (v_uv.x > 0.5) {
		o_color1 = pc.a;
	} else {
		o_color1 = pc.b;
	}
}
