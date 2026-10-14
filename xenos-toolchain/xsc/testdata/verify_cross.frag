#version 450
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(binding=0) uniform sampler2D u_tex;
layout(location=0) out vec4 o_color;
void main(){ vec3 c=cross(vec3(v_uv.x,v_uv.y,0.5),vec3(0.0,0.0,1.0)); o_color=vec4(c*0.5+0.5,1.0); }
