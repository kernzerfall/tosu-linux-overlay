#version 450

layout(push_constant) uniform PushConstants {
    vec4 rect;
    int mode;
    int srgb;
} pc;

layout(set = 0, binding = 0) uniform sampler2D overlay;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec4 c;
    if (pc.mode == 0) {
        // B8G8R8A8_UNORM image, premultiplied alpha
        c = texture(overlay, uv);
    } else {
        float d = length(uv - 0.5) * 2.0;
        float outer = 1.0 - smoothstep(0.85, 1.0, d);
        float inner = 1.0 - smoothstep(0.55, 0.7, d);
        c = vec4(vec3(inner), 1.0) * outer;
    }

    // the overlay is authored in sRGB, an sRGB target would encode it again
    if (pc.srgb != 0 && c.a > 0.0) {
        c.rgb = pow(c.rgb / c.a, vec3(2.2)) * c.a;
    }

    color = c;
}
