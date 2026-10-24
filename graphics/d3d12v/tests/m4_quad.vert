#version 450

// M4 test vertex shader (D3D clip space: +y up, z in [0,1]).
layout(location = 0) in vec3 pos;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 v_color;

void main()
{
    gl_Position = vec4(pos, 1.0);
    v_color = color;
}
