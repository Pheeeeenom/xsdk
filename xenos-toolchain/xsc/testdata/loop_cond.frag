#version 450
// if/else inside a loop body with a constant (induction-var) condition:
// the unroller picks the taken arm each iteration.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 8; i++) {
        if (i < 4)
            sum += vec4(0.05);
        else
            sum += vec4(0.10);
    }
    o_color = sum;
}
