#version 450
// A vertex shader with no varyings: only a position, from push constants.
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 scale; vec4 offset; } pc;

void main()
{
    gl_Position = a_position * pc.scale + pc.offset;
}
