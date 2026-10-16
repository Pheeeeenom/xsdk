#version 450
// A colour output at location 4, past the four colour targets.
layout(location = 0) out vec4 o_color0;
layout(location = 4) out vec4 o_color4;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	o_color0 = pc.a;
	o_color4 = pc.b;
}
