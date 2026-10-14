#version 450
// A triangle as a Vulkan app writes it: compiled with glslang -V, and with -G to compare.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;

void main()
{
    gl_Position = a_position;
    v_color = a_color;
}
