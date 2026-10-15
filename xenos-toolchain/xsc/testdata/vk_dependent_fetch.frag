#version 450
// A palette lookup, as Aurora's EFB copy conversion makes it: an index fetched, worked out, and the palette fetched at
// it. Laid out, the index's fetch and the work reading it share an exec, as do the work and the palette's fetch.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 1, binding = 0) uniform sampler2D palette;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 scale; } pc;

void main()
{
	vec4 index = floor(texture(source, v_uv) * 255.0 + 0.5);
	o_color = texture(palette, index.xx * pc.scale.xy + pc.scale.zw);
}
