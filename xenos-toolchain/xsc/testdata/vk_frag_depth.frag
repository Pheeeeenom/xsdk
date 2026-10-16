#version 450
// A pixel's depth from its coordinates, after a discard that keeps it, as Aurora's Z textures write theirs:
// o_color is a * uv.x, and gl_FragDepth uv.y * b.x + b.y.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
	o_color = pc.a * v_uv.x;
	if (pc.b.w > 0.5) {
		discard;
	}
	gl_FragDepth = v_uv.y * pc.b.x + pc.b.y;
}
