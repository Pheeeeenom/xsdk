#version 450
// samplerCube proof: direction comes from a vec3 interpolator.
layout(location = 0) in vec3 v_dir;
layout(binding = 0) uniform samplerCube u_cube;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = texture(u_cube, v_dir);
}
