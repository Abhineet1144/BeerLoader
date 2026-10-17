#version 450
layout(set = 0, binding = 0) readonly buffer Values {
    vec4 color;
} values;
layout(location = 0) out vec4 output_color;
void main()
{
    output_color = values.color;
}
