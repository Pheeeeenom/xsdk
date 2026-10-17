#version 450
// A depth compared at uv against the reference in z, times a push constant, so the comparison's register comes after
// the block's.
layout(location = 0) in vec3 v_uv_reference;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2DShadow u_shadow;
layout(push_constant) uniform Push { vec4 tint; } pc;

void main()
{
	o_color = vec4(texture(u_shadow, v_uv_reference)) * pc.tint;
}
