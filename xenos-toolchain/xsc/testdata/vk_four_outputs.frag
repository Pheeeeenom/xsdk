#version 450
// Four colour outputs, locations 0 to 3 to oC0 to oC3: two from the push constants, one from the coordinates and
// literals, one from both: o_color0 a, o_color1 b * uv.x, o_color2 (uv, 0.25, 2.5), o_color3 a + b.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color0;
layout(location = 1) out vec4 o_color1;
layout(location = 2) out vec4 o_color2;
layout(location = 3) out vec4 o_color3;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	o_color0 = pc.a;
	o_color1 = pc.b * v_uv.x;
	o_color2 = vec4(v_uv, 0.25, 2.5);
	o_color3 = pc.a + pc.b;
}
