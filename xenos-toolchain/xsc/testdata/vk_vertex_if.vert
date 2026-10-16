#version 450
// if/else on data in a vertex shader: arms storing a component of a local, an output and, through it, the position,
// one arm with an if of its own, and an && glslang makes a Phi of (tests/test_vertex_branches.py).
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(push_constant) uniform Push { vec4 offset; vec4 tint; } pc;

void main()
{
	vec4 p = a_position;
	if (a_color.w > 0.5) {
		p.x += pc.offset.x;
		v_color = a_color;
		if (a_color.x < 0.25)
			v_color = pc.tint;
	} else {
		p.y -= pc.offset.y;
		v_color = vec4(1.0, 0.0, 0.0, 1.0);
	}
	if (a_position.x > 0.0 && a_position.y < pc.offset.z)
		p.z = 0.5;
	gl_Position = p;
}
