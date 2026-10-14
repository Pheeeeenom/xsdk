#version 450
// A dependent texture read, as GX's indirect texturing makes: the image is sampled where the offsets texture says
// (tests/test_register_alloc.py).
layout(set = 1, binding = 0) uniform sampler2D offsets;
layout(set = 1, binding = 1) uniform sampler2D image;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 o = texture(offsets, v_uv);
    o_color = texture(image, v_uv + o.xy * 0.25);
}
