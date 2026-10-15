#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 1, binding = 0) uniform sampler2D palette;
layout(push_constant, std140) uniform Push {
    vec4 uv_transform;
    vec4 palette_scale;
} upush;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 index = floor(texture(source, in_uv) * 255.0 + 0.5);
    o_color = texture(palette, index.xx * upush.palette_scale.xy + upush.palette_scale.zw);
}
