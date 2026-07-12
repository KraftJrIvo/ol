#version 430

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

uniform sampler2D texture0;
uniform sampler2D texture1;

void main() {
    vec4 stableLighting = texture(texture0, fragTexCoord);
    float shadowMultiplier = texture(texture1, fragTexCoord).r;
    finalColor = vec4(stableLighting.rgb * shadowMultiplier, stableLighting.a) * fragColor;
}
