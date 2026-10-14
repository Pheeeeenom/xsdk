#version 450
// Exports straight from constants: a push constant as the colour.
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { layout(offset = 32) vec4 color; } pc;

void main()
{
    o_color = pc.color;
}
