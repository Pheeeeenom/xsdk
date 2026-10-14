#version 450
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(binding=0) uniform sampler2D u_tex;
layout(location=0) out vec4 o_color;
void main(){ if(v_uv.x>0.5){ if(v_uv.y>0.5) o_color=vec4(1.0,0.0,0.0,1.0); else o_color=vec4(0.0,1.0,0.0,1.0);} else o_color=vec4(0.0,0.0,1.0,1.0); }
