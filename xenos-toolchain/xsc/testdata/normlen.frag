#version 450
// normalize + length (scalar-pipe rsq/sqrt)
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec3 n = normalize(v_color.rgb);
    float l = length(v_color.rgb);
    o_color = vec4(n * l, 1.0);
}
