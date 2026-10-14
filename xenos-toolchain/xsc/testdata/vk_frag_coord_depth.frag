#version 450
// gl_FragCoord.z, which the pixel parameters don't hold: refused.
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(gl_FragCoord.z);
}
