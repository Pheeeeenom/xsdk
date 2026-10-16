#version 450
// Colour outputs at locations 3 and 1 alone, declared in that order, the second a float as an R32_SFLOAT attachment
// takes: o_far a * uv.y, o_red b.z + uv.x.
layout(location = 0) in vec2 v_uv;
layout(location = 3) out vec4 o_far;
layout(location = 1) out float o_red;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	o_far = pc.a * v_uv.y;
	o_red = pc.b.z + v_uv.x;
}
