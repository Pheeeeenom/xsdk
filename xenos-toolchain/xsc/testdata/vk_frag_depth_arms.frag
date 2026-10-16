#version 450
// gl_FragDepth stored in both arms of an if/else on data (tests/test_frag_depth.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	o_color = pc.a;
	if (v_uv.x > pc.b.x)
		gl_FragDepth = pc.b.y;
	else
		gl_FragDepth = v_uv.y;
}
