#version 450
// A colour picked at run time by the pixel's column, 128 pixels a band: gl_FragCoord from the pixel parameters, read
// at a0, as range fog reads its factors.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Bands { vec4 colors[10]; } u;

void main()
{
    o_color = u.colors[int(gl_FragCoord.x * 0.0078125)] * v_color;
}
