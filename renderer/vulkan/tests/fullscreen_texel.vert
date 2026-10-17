#version 450
layout(set = 0, binding = 0) uniform usamplerBuffer exposure_values;
layout(location = 0) out float exposure;
void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    exposure = uintBitsToFloat(texelFetch(exposure_values, 0).x);
}
