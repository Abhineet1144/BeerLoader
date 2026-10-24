#version 450

// M4 test pixel shader. CBV b0 (binding number is arbitrary: stages come
// from which shader declared the binding).
layout(set = 0, binding = 4, std140) uniform Tint { vec4 tint; };
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = v_color * tint;
}
