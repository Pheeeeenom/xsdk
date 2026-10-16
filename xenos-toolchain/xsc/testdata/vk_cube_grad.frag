#version 450
// textureGrad on a cube map, refused: the console hasn't shown which space a cube's register gradients are in.
layout(location = 0) in vec3 v_direction;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform samplerCube u_cube;

void main()
{
	o_color = textureGrad(u_cube, v_direction, vec3(0.01, 0.0, 0.0), vec3(0.0, 0.01, 0.0));
}
