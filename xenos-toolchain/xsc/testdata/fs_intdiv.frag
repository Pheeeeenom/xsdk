#version 450
// runtime integer division and modulo (SDiv / SMod) emulated on the float ALU.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main()
{
    int k = int(v_uv.x * 15.0);
    int q = k / 3;
    int r = k % 3;
    o_color = vec4(float(q) / 5.0, float(r) / 2.0, 0.0, 1.0);
}
