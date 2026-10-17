#version 450
// A cube's depth compared, which xsc refuses: it compares a sampler2DShadow's alone.
layout(location = 0) in vec4 v_direction_reference;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform samplerCubeShadow u_shadow;

void main()
{
	o_color = vec4(texture(u_shadow, v_direction_reference));
}
