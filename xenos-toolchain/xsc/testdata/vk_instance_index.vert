#version 450
// gl_InstanceIndex past a push block and a uniform block: a column of the index mod 4, a row of the index over 4, the
// index itself as depth, and a colour read at it.
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 scale; } pc;
layout(set = 0, binding = 0) uniform Colors { vec4 colors[8]; } u;

void main()
{
	vec2 cell = vec2(float(gl_InstanceIndex % 4), float(gl_InstanceIndex / 4)) * 0.5;
	gl_Position = a_position * pc.scale + vec4(cell, float(gl_InstanceIndex), 0.0);
	v_color = u.colors[gl_InstanceIndex];
}
