#version 450

layout(push_constant) uniform PushConstants {
    vec4 rect; // x0, y0, x1, y1 in NDC (y down)
    int mode;  // 0 = overlay texture, 1 = cursor
    int srgb;  // target is an sRGB format
} pc;

layout(location = 0) out vec2 uv;

void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    uv = p;
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, p), 0.0, 1.0);
}
