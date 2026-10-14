#version 450
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(binding=0) uniform sampler2D u_tex;
layout(location=0) out vec4 o_color;
void main(){
  float s = 0.0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      s += v_uv.x;                 // 3*3 = 9 -> 9*uv.x
  s = s / 9.0;
  o_color = vec4(s, s, s, 1.0);
}
