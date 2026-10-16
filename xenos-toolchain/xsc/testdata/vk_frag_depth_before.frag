#version 450
// gl_FragDepth stored before an if/else and again in one arm, with the colour (tests/test_frag_depth.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	o_color = pc.a;
	gl_FragDepth = pc.b.z;
	if (v_uv.y > pc.b.x) {
		gl_FragDepth = pc.b.w;
		o_color = pc.b;
	}
}
