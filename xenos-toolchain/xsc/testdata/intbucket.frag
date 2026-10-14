#version 450
// Float-backed integer proof: int(float_expr) + const, then float().
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    float scaled = dot(v_color, vec4(1.0, 0.0, 0.0, 0.0)) * 4.0;
    int bucket = int(scaled) + 1;
    float f = float(bucket) * 0.25;
    o_color = vec4(f, f, f, 1.0);
}
