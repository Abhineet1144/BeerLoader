#version 450
layout(location = 0) in float exposure;
layout(location = 0) out vec4 output_color;
void main()
{
    output_color = vec4(exposure, exposure * 0.5, exposure * 0.25, 1.0);
}
