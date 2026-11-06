#pragma once
// How a hair card vertex carries its data to HairCardPS (src/shaders/mesh.hlsl). Vertex alpha 0.68 marks hair; the other three colour
// channels are float32, so each packs an integer part that is constant across a card (never interpolated across a boundary) with a
// fraction that may vary linearly along it. Decoded by HairDecode in mesh.hlsl; keep the two in step.
//
//   r = floor(cardRand * 1000) + t * 0.999        t: root 0 .. tip 1 along the strand, cardRand: 0..1 per card
//   g = cls + depth * 0.999                        cls: kHairScalp / kHairFine (brows, lashes) / kHairBeard; depth: 0 at the
//                                                  outside of the hair volume .. 1 deep inside it (by the skin)
//   b = 0                                           colour from the texture's RGB (brow and lash strand sheets)
//   b = floor(red * 100) * 100 + floor(grey * 99) + melanin * 0.999
//                                                  colour from melanin (eumelanin 0 blond .. 1 black, red = pheomelanin share,
//                                                  grey = share of white strands); the texture is then the data atlas
//                                                  (gameenginetools/hair_atlas: R brightness, G root-tip, B strand random, A coverage)
#include <algorithm>
#include <cmath>

namespace tbd {

constexpr float kHairAlpha = 0.68f;
// kHairCap: the shrunk clay under the cards (gameenginetools/hair_groom/cap.py): t carries its coverage (0 at the hairline), uv its
// flow coordinates (across, along) in metres, whose screen-space gradient gives the strand direction.
// kHairShell: a fur shell over the hairline band (AddHairShell): t is the shell's height 0..1, green's fraction the fur density,
// uv the band's flow coordinates in metres.
// kHairWet: a card of wet tissue, not hair (the caruncle and the tear line, lash_cards.py "cls": 5): its sheet colour, lit glossy.
constexpr int kHairScalp = 0, kHairFine = 1, kHairBeard = 2, kHairCap = 3, kHairShell = 4, kHairWet = 5;

struct HairTint {
    float melanin = 0.45f; // 0 platinum, 0.2 blond, 0.4 light brown, 0.6 brown, 0.8 dark brown, 1 black
    float red = 0.12f;     // pheomelanin share: 0 ash, 0.3 warm brown, 0.8 auburn/ginger
    float grey = 0.0f;     // share of unpigmented strands
};

inline float HairPackR(float t, float cardRand) {
    return std::floor(std::clamp(cardRand, 0.0f, 0.9999f) * 1000.0f) + std::clamp(t, 0.0f, 1.0f) * 0.999f;
}
inline float HairPackG(int cls, float depth) { return (float)cls + std::clamp(depth, 0.0f, 1.0f) * 0.999f; }
inline float HairPackB(const HairTint* tint) {
    if (!tint) return 0.0f;
    const float red = std::floor(std::clamp(tint->red, 0.0f, 0.99f) * 100.0f), grey = std::floor(std::clamp(tint->grey, 0.0f, 1.0f) * 99.0f);
    return red * 100.0f + grey + std::clamp(tint->melanin, 0.001f, 1.0f) * 0.999f;
}
// Hair vertices carry data in their colour: entity tints must leave them alone.
inline bool IsHairVertex(float alpha) { return std::fabs(alpha - kHairAlpha) < 0.003f; }
// The fraction of r (root to tip), for code that reads hair vertices back.
inline float HairT(float r) { return std::clamp((r - std::floor(r)) / 0.999f, 0.0f, 1.0f); }

} // namespace tbd
