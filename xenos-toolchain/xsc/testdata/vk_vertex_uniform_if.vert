#version 450
// An if/else on a push constant: every vertex takes the same arm (tests/test_vertex_branches.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; float mode; } pc;

void main()
{
	vec4 p = a_position * pc.scale;
	if (pc.mode > 0.5) {
		p.xy = p.yx;
		p += pc.offset;
		v_color = vec4(0.0, 1.0, 0.0, 1.0);
	} else {
		v_color = a_color * pc.scale.z;
	}
	gl_Position = p;
}
