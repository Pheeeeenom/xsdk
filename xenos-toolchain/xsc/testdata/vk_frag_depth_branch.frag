#version 450
// gl_FragDepth stored in one arm of an if/else, which the lowering would export on every path.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	if (v_uv.x > pc.b.x) {
		o_color = pc.a;
		gl_FragDepth = pc.b.y;
	} else {
		o_color = pc.b;
	}
}
