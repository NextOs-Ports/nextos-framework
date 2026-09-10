#version 450
layout(location=0) in vec2 uv;
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) out vec4 output_color;
void main() {
    vec4 color = texture(image, uv);
    output_color = vec4(color.rgb * color.a, color.a);
}
