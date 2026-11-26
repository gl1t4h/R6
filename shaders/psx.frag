#version 330 core

// Native presentation of the PlayStation display area.
//
// The guest GPU renders into 15-bit VRAM. The host samples that display area
// with nearest filtering so the original pixel grid survives, then applies the
// optional scanline mask and a gamma lift for the 5-bit-per-channel colour
// range. Set u_scanlines to 0.0 for a clean upscale.
uniform sampler2D u_texture;
uniform vec2 u_source_size;
uniform float u_scanlines;
uniform float u_gamma;

in vec2 v_uv;
out vec4 o_color;

void main() {
    vec2 pixel = v_uv * u_source_size;
    vec2 uv = (floor(pixel) + 0.5) / u_source_size;
    vec3 color = texture(u_texture, uv).rgb;
    float line = 0.5 + 0.5 * cos(pixel.y * 3.14159265);
    color *= mix(1.0, line, u_scanlines);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / u_gamma));
    o_color = vec4(color, 1.0);
}
