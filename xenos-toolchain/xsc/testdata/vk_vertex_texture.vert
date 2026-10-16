#version 450
// Textures in a vertex shader, as terrain samples a height map: texture() at LOD 0 at the vertex's coordinates moves it
// up, and textureLod at a LOD the vertex works out gives its colour, from the second sampler.
layout(location = 0) in vec4 a_position;
layout(location = 2) in vec2 a_uv;
layout(set = 0, binding = 0) uniform sampler2D u_height;
layout(set = 0, binding = 1) uniform sampler2D u_colour;
layout(location = 0) out vec4 v_color;

void main()
{
	vec4 height = texture(u_height, a_uv);
	gl_Position = a_position + vec4(0.0, height.x, 0.0, 0.0);
	v_color = textureLod(u_colour, a_uv, a_position.z * 4.0);
}
