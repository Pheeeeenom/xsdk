#version 450
// One local through 64 multiply-adds, each reading the last.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 k; vec4 c; } pc;

#define M x = x * pc.k + pc.c;
#define M8 M M M M M M M M

void main()
{
	vec4 x = v_uv.xyxy;
	M8 M8 M8 M8 M8 M8 M8 M8
	o_color = x;
}
