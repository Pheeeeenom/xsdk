#version 450
// Integer scalar proof: int locals fold through float() into color math.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    int base = 2;
    int scale_i = base * 3 + 1;
    float scale = float(scale_i) * 0.125;
    o_color = v_color * scale;
}
