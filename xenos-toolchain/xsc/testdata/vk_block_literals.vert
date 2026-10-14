#version 450
// A push block, a uniform block and literals: the uniform block has its groups of 4 to itself, the literals after it.
layout(location = 0) in vec2 a_position;
layout(push_constant) uniform Push { vec2 offset; } push;
layout(set = 0, binding = 0) uniform Scale { vec2 scale; } u;

void main()
{
	gl_Position = vec4(a_position * u.scale + push.offset, 0.5, 1.0);
}
