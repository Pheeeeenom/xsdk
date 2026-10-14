#version 450
// transpose + determinant of a mat3 uniform.
layout(location = 0) in vec4 v_color;
layout(location = 0) uniform mat3 u_m;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(transpose(u_m) * v_color.xyz, determinant(u_m));
}
