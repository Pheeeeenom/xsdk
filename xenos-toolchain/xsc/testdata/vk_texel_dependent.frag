#version 450
// A fetch whose coordinates come from another's result, beside two that don't: the dependent one has to wait for its
// source's fetch, and the independent ones may go with that first one.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_offsets;
layout(set = 0, binding = 1) uniform sampler2D u_texture;
layout(push_constant) uniform Push { vec4 delta; } pc;

void main()
{
	vec4 offset = texture(u_offsets, v_uv);
	vec4 moved = texture(u_texture, v_uv + offset.xy * pc.delta.w);
	vec4 across = texture(u_texture, v_uv + pc.delta.xy);
	o_color = offset + moved + across;
}
