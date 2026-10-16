#version 450
// Floats read out of vectors that are values, not variables, as Aurora's TEV comparisons read a splatted alpha's red:
// o_color is a.w * 2, the y of a + b * uv.x times b.z, a.w's byte (rounded) over 255, and 1.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	vec4 sum = pc.a + pc.b * v_uv.x;
	o_color = vec4((vec3(pc.a.w)).r * 2.0, sum.y * pc.b.z, floor((vec3(pc.a.w)).r * 255.0 + 0.5) / 255.0, 1.0);
}
