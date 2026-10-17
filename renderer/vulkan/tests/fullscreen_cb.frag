#version 450
layout(set = 0, binding = 32768) uniform Params {
    vec4 color;
} params;
layout(location = 0) out vec4 output_color;
void main() {
    output_color = params.color;
}
