#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform vec4 u_n;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 r = reflect(v_color.rgb, normalize(u_n.rgb));
    float e = exp(v_color.r);
    o_color = vec4(r * e + vec3(sin(v_color.g)), 1.0);
}
