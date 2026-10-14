#version 450
// Forty dot products, all live until a sum in reverse reads them: more values than r0..r63 hold unless the
// allocator recycles dead temporaries (tests/test_register_alloc.py). Two inputs, so a fetch reads the vertex
// index from r0 before the last fetch writes it.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_scale;
layout(set = 0, binding = 0) uniform Weights { vec4 w[40]; } u;
layout(location = 0) out vec4 v_sum;

void main()
{
    float d0 = dot(u.w[0], a_position);
    float d1 = dot(u.w[1], a_position);
    float d2 = dot(u.w[2], a_position);
    float d3 = dot(u.w[3], a_position);
    float d4 = dot(u.w[4], a_position);
    float d5 = dot(u.w[5], a_position);
    float d6 = dot(u.w[6], a_position);
    float d7 = dot(u.w[7], a_position);
    float d8 = dot(u.w[8], a_position);
    float d9 = dot(u.w[9], a_position);
    float d10 = dot(u.w[10], a_position);
    float d11 = dot(u.w[11], a_position);
    float d12 = dot(u.w[12], a_position);
    float d13 = dot(u.w[13], a_position);
    float d14 = dot(u.w[14], a_position);
    float d15 = dot(u.w[15], a_position);
    float d16 = dot(u.w[16], a_position);
    float d17 = dot(u.w[17], a_position);
    float d18 = dot(u.w[18], a_position);
    float d19 = dot(u.w[19], a_position);
    float d20 = dot(u.w[20], a_position);
    float d21 = dot(u.w[21], a_position);
    float d22 = dot(u.w[22], a_position);
    float d23 = dot(u.w[23], a_position);
    float d24 = dot(u.w[24], a_position);
    float d25 = dot(u.w[25], a_position);
    float d26 = dot(u.w[26], a_position);
    float d27 = dot(u.w[27], a_position);
    float d28 = dot(u.w[28], a_position);
    float d29 = dot(u.w[29], a_position);
    float d30 = dot(u.w[30], a_position);
    float d31 = dot(u.w[31], a_position);
    float d32 = dot(u.w[32], a_position);
    float d33 = dot(u.w[33], a_position);
    float d34 = dot(u.w[34], a_position);
    float d35 = dot(u.w[35], a_position);
    float d36 = dot(u.w[36], a_position);
    float d37 = dot(u.w[37], a_position);
    float d38 = dot(u.w[38], a_position);
    float d39 = dot(u.w[39], a_position);
    float sum = d39 + d38 + d37 + d36 + d35 + d34 + d33 + d32 + d31 + d30 + d29 + d28 + d27 + d26 + d25 + d24 + d23 + d22 + d21 + d20 + d19 + d18 + d17 + d16 + d15 + d14 + d13 + d12 + d11 + d10 + d9 + d8 + d7 + d6 + d5 + d4 + d3 + d2 + d1 + d0;
    float alternating = d0 * 1.0 + d1 * -1.0 + d2 * 1.0 + d3 * -1.0 + d4 * 1.0 + d5 * -1.0 + d6 * 1.0 + d7 * -1.0 + d8 * 1.0 + d9 * -1.0 + d10 * 1.0 + d11 * -1.0 + d12 * 1.0 + d13 * -1.0 + d14 * 1.0 + d15 * -1.0 + d16 * 1.0 + d17 * -1.0 + d18 * 1.0 + d19 * -1.0 + d20 * 1.0 + d21 * -1.0 + d22 * 1.0 + d23 * -1.0 + d24 * 1.0 + d25 * -1.0 + d26 * 1.0 + d27 * -1.0 + d28 * 1.0 + d29 * -1.0 + d30 * 1.0 + d31 * -1.0 + d32 * 1.0 + d33 * -1.0 + d34 * 1.0 + d35 * -1.0 + d36 * 1.0 + d37 * -1.0 + d38 * 1.0 + d39 * -1.0;
    gl_Position = a_position;
    v_sum = vec4(sum * a_scale.x, alternating, d0, d39 * a_scale.y);
}
