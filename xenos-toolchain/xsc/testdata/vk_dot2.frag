#version 450
// Dot products of vec2s whose registers hold more past y: o_color is a.xy . b.xy, a.zw . b.zw, (a.xy * uv) . b.xy, 1.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	vec2 p = pc.a.xy * v_uv;
	o_color = vec4(dot(pc.a.xy, pc.b.xy), dot(pc.a.zw, pc.b.zw), dot(p, pc.b.xy), 1.0);
}
