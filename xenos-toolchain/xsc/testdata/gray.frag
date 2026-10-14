#version 450
// luminance desaturation (dimmed/disabled UI look) — exercises dot()
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    float lum = dot(tex.rgb, vec3(0.299, 0.587, 0.114));
    o_color = vec4(lum, lum, lum, tex.a);
}
