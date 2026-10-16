// Aurora GLSL for GX: sixteen TEV stages with indirect texturing, 14 fetches; with its fetches laid out together
// it needs past the 64 registers xsdk gives a pixel shader (vulkan4xenon milestone 18, random config 932).
#version 450
layout(set = 0, binding = 0, std140) uniform View {
    vec4 viewport_sizes; // render, then logical
    vec4 line_params;
    vec4 proj[4];
    vec4 tex_mtx[30];
    vec4 channel_colors[8]; // per channel: ambient, material, alpha ambient, alpha material
    vec4 postmtx[60];
    vec4 fog[5];
    vec4 ind_mtx[6];
} uview;
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(set = 1, binding = 2) uniform sampler2D tex2;
layout(set = 1, binding = 3) uniform sampler2D tex3;
layout(set = 1, binding = 4) uniform sampler2D tex4;
layout(set = 1, binding = 5) uniform sampler2D tex5;
layout(set = 1, binding = 6) uniform sampler2D tex6;
layout(set = 1, binding = 7) uniform sampler2D tex7;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec4 in_cc1;
layout(location = 2) in vec2 in_tex0_uv;
layout(location = 3) in vec2 in_tex1_uv;
layout(location = 4) in vec2 in_tex2_uv;
layout(location = 5) in vec2 in_tex3_uv;
layout(location = 6) in vec3 in_tex4_uvw;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec2 tex1_uv = in_tex1_uv;
    vec2 tex2_uv = in_tex2_uv;
    vec2 tex3_uv = in_tex3_uv;
    float tex4_near = step(abs(in_tex4_uvw.z), 1e-6);
    vec2 tex4_uv = in_tex4_uvw.xy / mix(in_tex4_uvw.z, 1.0, tex4_near);
    // Indirect stage 0
    vec3 ind_tex0 = 255.0 * texture(tex5, tex0_uv * umat.texcoord_scale[0].xy * vec2((1.0 / 8.0), (1.0 / 4.0)) / umat.tex_size_bias[5].xy).abg;
    // Indirect stage 1
    vec3 ind_tex1 = 255.0 * texture(tex1, tex2_uv * umat.texcoord_scale[2].xy * vec2((1.0 / 4.0), (1.0 / 4.0)) / umat.tex_size_bias[1].xy).abg;
    // Indirect stage 2
    vec3 ind_tex2 = 255.0 * texture(tex7, tex1_uv * umat.texcoord_scale[1].xy * vec2((1.0 / 64.0), (1.0 / 8.0)) / umat.tex_size_bias[7].xy).abg;
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(umat.tevreg[2].a)), (vec3(1.0)), (vec3(umat.tevreg[1].a))) + umat.tevreg[2].rgb) - 0.5), 0.0, 1.0);
    float s0_alpha = clamp(((umat.tevreg[2].a) * step(0.5, floor(dot((vec3(umat.tevreg[2].a)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((vec3(1.0)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)) + 0.0), 0.0, 1.0);
    vec4 sampled1 = texture(tex3, tex3_uv);
    // TEV stage 1
    vec3 s1_color = clamp(((vec3(0.0)) * (1.0 - step(0.5, abs(floor(dot((vec3(1.0)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((vec3(0.5)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)))) + vec3(1.0)), 0.0, 1.0);
    float s1_alpha = clamp(((0.0) * step(0.5, floor(dot((vec3(1.0)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((vec3(0.5)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)) + 0.0), 0.0, 1.0);
    // TEV stage 2 indirect
    vec3 ind2_coord = ind_tex2;
    vec3 ind2_biased = ind2_coord + vec3(0.0, 1.0, 0.0) * -128.0;
    vec2 ind2_texel = tex2_uv * umat.texcoord_scale[2].xy;
    vec2 tev_coord2 = vec2(0.0) + (vec2(ind2_texel.x, (ind2_texel.y - 32.0 * trunc(ind2_texel.y / 32.0))) + (tex2_uv * umat.texcoord_scale[2].xy * ind2_biased.y * uview.ind_mtx[1].z / 256.0));
    vec2 ind2_uv = tev_coord2 / umat.tex_size_bias[4].xy;
    vec4 sampled2 = texture(tex4, ind2_uv);
    // TEV stage 2
    vec3 s2_color = clamp(((vec3(0.5)) * (1.0 - step(0.5, abs(floor(dot((vec3(sampled2.a)).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((s1_color).rg * 255.0, vec2(1.0, 256.0)) + 0.5)))) + vec3(umat.tevreg[0].a)), 0.0, 1.0);
    float s2_alpha = clamp(((s0_alpha) * step(0.5, floor(dot((vec3(sampled2.a)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((s1_color).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)) + in_cc1.a), -4.0, 4.0);
    // TEV stage 3 indirect
    vec3 ind3_coord = floor(ind_tex0 / 8.0);
    vec3 ind3_biased = ind3_coord + vec3(1.0, 1.0, 1.0) * 1.0;
    vec2 ind3_texel = tex3_uv * umat.texcoord_scale[3].xy;
    vec2 tev_coord3 = vec2((ind3_texel.x - 32.0 * trunc(ind3_texel.x / 32.0)), (ind3_texel.y - 16.0 * trunc(ind3_texel.y / 16.0))) + (tex3_uv * umat.texcoord_scale[3].xy * ind3_biased.y * uview.ind_mtx[1].z / 256.0);
    // TEV stage 3
    vec3 s3_color = clamp(((umat.tevreg[1].rgb) * step(0.5, floor((vec3(s0_alpha)).r * 255.0 + 0.5) - floor((vec3(umat.tevreg[0].a)).r * 255.0 + 0.5)) + umat.tevreg[1].rgb), 0.0, 1.0);
    float s3_alpha = clamp((((1.0 / 8.0)) * step(0.5, floor(dot((vec3(s0_alpha)).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((vec3(umat.tevreg[0].a)).rg * 255.0, vec2(1.0, 256.0)) + 0.5)) + 0.0), -4.0, 4.0);
    // TEV stage 4 indirect
    vec3 ind4_coord = floor(ind_tex2 / 16.0);
    vec3 ind4_biased = ind4_coord + vec3(1.0, 0.0, 1.0) * 1.0;
    vec2 ind4_texel = tex1_uv * umat.texcoord_scale[1].xy;
    vec2 tev_coord4 = tev_coord3 + (vec2((ind4_texel.x - 256.0 * trunc(ind4_texel.x / 256.0)), ind4_texel.y) + (tex1_uv * umat.texcoord_scale[1].xy * ind4_biased.x * uview.ind_mtx[5].z / 256.0));
    vec2 ind4_uv = tev_coord4 / umat.tex_size_bias[0].xy;
    vec4 sampled4 = texture(tex0, ind4_uv);
    // TEV stage 4
    vec3 s4_color = clamp(((mix((s3_color), (sampled4.rbb), (umat.kcolor[3].rgb)) + vec3(0.0))) * 2.0, 0.0, 1.0);
    float s4_alpha = clamp(((sampled4.a) * step(0.5, floor(dot((s3_color).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((sampled4.rbb).rg * 255.0, vec2(1.0, 256.0)) + 0.5)) + s0_alpha), 0.0, 1.0);
    vec4 sampled5 = texture(tex6, tex0_uv);
    // TEV stage 5
    vec3 s5_color = clamp(((s2_color - mix((s0_color), (vec3(0.5)), (sampled5.bgb)))) / 2.0, 0.0, 1.0);
    float s5_alpha = clamp(((umat.kcolor[1].a) * (1.0 - step(0.5, abs(floor((0.0) * 255.0 + 0.5) - floor((umat.kcolor[1].a) * 255.0 + 0.5)))) + s0_alpha), 0.0, 1.0);
    // TEV stage 6 indirect
    vec3 ind6_coord = floor(ind_tex1 / 8.0);
    vec3 ind6_biased = ind6_coord + vec3(0.0, 1.0, 0.0) * 1.0;
    vec4 ind6_c0 = uview.ind_mtx[0];
    vec4 ind6_c1 = uview.ind_mtx[1];
    vec2 ind6_texel = tex4_uv * umat.texcoord_scale[4].xy;
    vec2 tev_coord6 = vec2(0.0, (ind6_texel.y - 256.0 * trunc(ind6_texel.y / 256.0))) + (vec2(dot(vec3(ind6_c0.xz, ind6_c1.x), ind6_biased), dot(vec3(ind6_c0.yw, ind6_c1.y), ind6_biased)) * ind6_c1.z);
    vec2 ind6_uv = tev_coord6 / umat.tex_size_bias[4].xy;
    vec4 sampled6 = texture(tex4, ind6_uv);
    // TEV stage 6
    vec3 s6_color = clamp(((vec3(0.0)) * (1.0 - step(0.5, abs(floor((s2_color).r * 255.0 + 0.5) - floor((s2_color).r * 255.0 + 0.5)))) + umat.kcolor[3].rgb), 0.0, 1.0);
    float s6_alpha = clamp(((sampled6.a) * (1.0 - step(0.5, abs(floor(dot((s2_color).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((s2_color).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)))) + s5_alpha), 0.0, 1.0);
    // TEV stage 7 indirect
    vec3 ind7_coord = floor(ind_tex2 / 32.0);
    vec3 ind7_biased = ind7_coord + vec3(1.0, 1.0, 1.0) * 1.0;
    vec4 ind7_c0 = uview.ind_mtx[2];
    vec4 ind7_c1 = uview.ind_mtx[3];
    vec2 ind7_texel = tex3_uv * umat.texcoord_scale[3].xy;
    vec2 tev_coord7 = tev_coord6 + (vec2((ind7_texel.x - 256.0 * trunc(ind7_texel.x / 256.0)), (ind7_texel.y - 16.0 * trunc(ind7_texel.y / 16.0))) + (vec2(dot(vec3(ind7_c0.xz, ind7_c1.x), ind7_biased), dot(vec3(ind7_c0.yw, ind7_c1.y), ind7_biased)) * ind7_c1.z));
    vec2 ind7_uv = tev_coord7 / umat.tex_size_bias[4].xy;
    vec4 sampled7 = texture(tex4, ind7_uv);
    // TEV stage 7
    vec3 s7_color = clamp(((s3_color) * (1.0 - step(0.5, abs(floor((vec3(0.5)).r * 255.0 + 0.5) - floor((vec3(0.5)).r * 255.0 + 0.5)))) + s6_color), 0.0, 1.0);
    float s7_alpha = clamp(((in_cc0.a) * (1.0 - step(0.5, abs(floor(dot((vec3(0.5)).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((vec3(0.5)).rg * 255.0, vec2(1.0, 256.0)) + 0.5)))) + sampled7.a), 0.0, 1.0);
    // TEV stage 8 indirect
    vec3 ind8_coord = floor(ind_tex2 / 8.0);
    vec3 ind8_biased = ind8_coord + vec3(1.0, 1.0, 1.0) * 1.0;
    vec2 ind8_texel = tex3_uv * umat.texcoord_scale[3].xy;
    vec2 tev_coord8 = vec2((ind8_texel.x - 32.0 * trunc(ind8_texel.x / 32.0)), (ind8_texel.y - 256.0 * trunc(ind8_texel.y / 256.0))) + (tex3_uv * umat.texcoord_scale[3].xy * ind8_biased.y * uview.ind_mtx[5].z / 256.0);
    // TEV stage 8
    vec3 s8_color = clamp(((vec3((((floor(ind_tex2.y) - mod(floor(ind_tex2.y), 32.0)) / 255.0) * (255.0 / 248.0)))) * step(0.5, floor(dot((s7_color).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((vec3(s6_alpha)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)) + vec3(umat.kcolor[0].b)), 0.0, 1.0);
    float s8_alpha = clamp(((s7_alpha) * (1.0 - step(0.5, abs(floor((s7_alpha) * 255.0 + 0.5) - floor(((((floor(ind_tex2.y) - mod(floor(ind_tex2.y), 32.0)) / 255.0) * (255.0 / 248.0))) * 255.0 + 0.5)))) + s7_alpha), 0.0, 1.0);
    // TEV stage 9 indirect
    vec3 ind9_coord = floor(ind_tex0 / 32.0);
    vec3 ind9_biased = ind9_coord + vec3(0.0, 0.0, 1.0) * 1.0;
    vec2 ind9_texel = tex3_uv * umat.texcoord_scale[3].xy;
    vec2 tev_coord9 = vec2((ind9_texel.x - 32.0 * trunc(ind9_texel.x / 32.0)), (ind9_texel.y - 16.0 * trunc(ind9_texel.y / 16.0))) + (tex3_uv * umat.texcoord_scale[3].xy * ind9_biased.y * uview.ind_mtx[1].z / 256.0);
    vec2 ind9_uv = tev_coord9 / umat.tex_size_bias[1].xy;
    vec4 sampled9 = texture(tex1, ind9_uv);
    // TEV stage 9
    vec3 s9_color = clamp(((vec3(s8_alpha)) * (1.0 - step(0.5, abs(floor(dot((vec3(s0_alpha)).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((vec3(0.5)).rg * 255.0, vec2(1.0, 256.0)) + 0.5)))) + vec3(0.0)), 0.0, 1.0);
    float s9_alpha = clamp(((mix((sampled9.a), (s6_alpha), (s8_alpha)) + s0_alpha) + 0.5) * 2.0, 0.0, 1.0);
    // TEV stage 10 indirect
    vec3 ind10_coord = floor(ind_tex1 / 32.0);
    vec3 ind10_biased = ind10_coord + vec3(0.0, 1.0, 0.0) * 1.0;
    vec2 ind10_texel = tex1_uv * umat.texcoord_scale[1].xy;
    vec2 tev_coord10 = tev_coord9 + (vec2((ind10_texel.x - 16.0 * trunc(ind10_texel.x / 16.0)), ind10_texel.y) + (tex1_uv * umat.texcoord_scale[1].xy * ind10_biased.y * uview.ind_mtx[3].z / 256.0));
    vec2 ind10_uv = tev_coord10 / umat.tex_size_bias[2].xy;
    vec4 sampled10 = texture(tex2, ind10_uv);
    // TEV stage 10
    vec3 s10_color = clamp(((sampled10.rgb) * (1.0 - step(0.5, abs(floor(dot((vec3(0.0)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((s7_color).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)))) + vec3(s0_alpha)), 0.0, 1.0);
    float s10_alpha = clamp(((s9_alpha) * (1.0 - step(0.5, abs(floor((0.0) * 255.0 + 0.5) - floor(((7.0 / 8.0)) * 255.0 + 0.5)))) + 0.0), 0.0, 1.0);
    // TEV stage 11 indirect
    // TEV stage 11
    vec3 s11_color = clamp(((mix((vec3(1.0)), (s7_color), (vec3(s7_alpha))) + in_cc1.rga) + 0.5), 0.0, 1.0);
    float s11_alpha = clamp(((s7_alpha - mix((s6_alpha), (1.0), (s6_alpha))) + 0.5) * 4.0, -4.0, 4.0);
    vec4 sampled12 = texture(tex3, tex4_uv);
    // TEV stage 12
    vec3 s12_color = clamp(((vec3(s10_alpha)) * step(0.5, floor(dot((vec3(0.0)).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((vec3(1.0)).rg * 255.0, vec2(1.0, 256.0)) + 0.5)) + vec3(0.0)), 0.0, 1.0);
    float s12_alpha = clamp((((2.0 / 8.0)) * (1.0 - step(0.5, abs(floor((sampled12.a) * 255.0 + 0.5) - floor((s6_alpha) * 255.0 + 0.5)))) + s10_alpha), 0.0, 1.0);
    // TEV stage 13
    vec3 s13_color = clamp(((vec3(1.0)) * step(0.5, floor(dot((vec3(in_cc0.a)).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((vec3(in_cc0.a)).rg * 255.0, vec2(1.0, 256.0)) + 0.5)) + vec3(s7_alpha)), -4.0, 4.0);
    float s13_alpha = clamp(((s10_alpha) * step(0.5, floor(dot((vec3(in_cc0.a)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5) - floor(dot((vec3(in_cc0.a)).rgb * 255.0, vec3(1.0, 256.0, 65536.0)) + 0.5)) + s12_alpha), 0.0, 1.0);
    vec4 sampled14 = texture(tex7, tex1_uv);
    // TEV stage 14
    vec3 s14_color = clamp(((vec3(7.0 / 8.0)) * (1.0 - step(0.5, abs(floor(dot((vec3(s7_alpha)).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((s10_color).rg * 255.0, vec2(1.0, 256.0)) + 0.5)))) + vec3(0.0)), 0.0, 1.0);
    float s14_alpha = clamp(((0.0 - mix((sampled14.a), (s10_alpha), (s12_alpha))) + 0.5) * 2.0, 0.0, 1.0);
    // TEV stage 15 indirect
    vec3 ind15_coord = ind_tex1;
    vec3 ind15_biased = ind15_coord + vec3(1.0, 1.0, 0.0) * -128.0;
    vec4 ind15_c0 = uview.ind_mtx[4];
    vec4 ind15_c1 = uview.ind_mtx[5];
    vec2 ind15_texel = tex0_uv * umat.texcoord_scale[0].xy;
    vec2 tev_coord15 = vec2((ind15_texel.x - 128.0 * trunc(ind15_texel.x / 128.0)), ind15_texel.y) + (vec2(dot(vec3(ind15_c0.xz, ind15_c1.x), ind15_biased), dot(vec3(ind15_c0.yw, ind15_c1.y), ind15_biased)) * ind15_c1.z);
    vec2 ind15_uv = tev_coord15 / umat.tex_size_bias[3].xy;
    vec4 sampled15 = texture(tex3, ind15_uv);
    // TEV stage 15
    vec3 s15_color = clamp(((vec3(0.0)) * step(0.5, floor(dot((s14_color).rg * 255.0, vec2(1.0, 256.0)) + 0.5) - floor(dot((vec3(0.0)).rg * 255.0, vec2(1.0, 256.0)) + 0.5)) + sampled15.bgb), -4.0, 4.0);
    float s15_alpha = clamp(((mix((0.0), (0.0), (s7_alpha)) + s14_alpha)), -4.0, 4.0);
    vec3 out_color = ((s15_color) * 255.0 - floor((s15_color) * 255.0 / 256.0) * 256.0) / 255.0;
    float out_alpha = ((s15_alpha) * 255.0 - floor((s15_alpha) * 255.0 / 256.0) * 256.0) / 255.0;
    o_color = vec4(out_color, out_alpha);
}
