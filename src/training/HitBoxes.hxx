#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// The fighters' collision boxes as the game holds them during a fight, and the
// camera they are drawn through. Owns only the model and the byte layout of
// one box; it knows nothing of the game or the overlay.
namespace sf4e { namespace training {
// Hurt: where a strike lands. Body: where a throw connects, and about where
// fighters stop each other. Hit, Throw: an attack's, on the frames it can touch.
enum class BoxKind : std::uint8_t { Hurt, Body, Hit, Throw };
// A box in the fight's own units, on its plane (z = 0): its four corners, in
// order round it. Not always upright: a box on a leaning body leans with it,
// and the upright rectangle round it reaches further than the box does.
// Against Dan, whose stance leans, that was up to a tenth of a unit of reach
// that the game did not give. side: whose it is, 0 or 1.
struct Box { float x[4], y[4]; BoxKind kind; std::uint8_t side; };
// How far an attack box of that kind of the attacking side reaches into a box
// of the other fighter that it can connect with: a strike into a Hurt box, a
// throw into a Body box. Above zero they overlap; below, it is about the gap
// between the nearest two. -1000 when there is no such pair. attacker: 0 or
// 1, or -1 for either.
inline float TouchDepth(const std::vector<Box>& boxes, BoxKind attack, int attacker = -1) {
    const BoxKind target = attack == BoxKind::Throw ? BoxKind::Body : BoxKind::Hurt;
    float depth = -1000;
    for (const Box& a : boxes) if (a.kind == attack && (attacker < 0 || a.side == attacker))
        for (const Box& b : boxes) if (b.kind == target && b.side != a.side) {
            // Two boxes overlap when their shadows do along every side of
            // either; the least of those overlaps is how far in they are.
            float pair = 1000;
            for (const Box* of : {&a, &b}) for (int side = 0; side < 2; ++side) {
                float nx = of->y[side] - of->y[side + 1], ny = of->x[side + 1] - of->x[side];
                const float length = std::sqrt(nx * nx + ny * ny);
                if (length < 1e-6f) continue;
                nx /= length; ny /= length;
                float low[2], high[2];
                const Box* both[2] = {&a, &b};
                for (int which = 0; which < 2; ++which) for (int corner = 0; corner < 4; ++corner) {
                    const float at = both[which]->x[corner] * nx + both[which]->y[corner] * ny;
                    if (!corner || at < low[which]) low[which] = at;
                    if (!corner || at > high[which]) high[which] = at;
                }
                pair = (std::min)(pair, (std::min)(high[0], high[1]) - (std::max)(low[0], low[1]));
            }
            if (pair > depth) depth = pair;
        }
    return depth;
}
inline bool Touches(const std::vector<Box>& boxes, BoxKind attack) { return TouchDepth(boxes, attack) > 0; }

// view, projection: the game's camera matrices, 16 floats each, rows first.
// valid: both were read this frame.
struct BoxView { bool valid = false; float view[16] = {}, projection[16] = {}; std::vector<Box> boxes; };

// Which parts of the body a move's script leaves open on a frame, one bit a
// part: waist, stomach, chest, head, then shoulder, elbow, wrist and hand of
// the left arm, hip, knee, ankle and foot of the left leg, and the same of
// the right side. script: the game's own script of the fighter's action (a
// 0x18-byte header, then its command lists). List 8 holds the commands that
// say it: each covers frames from a first up to a last, and names what the
// listed parts are open to (2 a strike, 4 a throw, 8 a fireball). A command
// naming no parts is the invincibility of a Shoryuken's first frames. With no
// command on the frame, every part is open: the limb an attack held out, on
// its way back. Checked against the in-game hitboxes test: 487 of 488 tries
// of four matchups come out as this says.
constexpr std::uint32_t AllParts = 0xFFFFFF, OpenToStrike = 2, OpenToThrow = 4;
// Wrist, hand, ankle and foot have no box of their own in the game: a foot
// that reached the attack and nothing else was never hit.
constexpr std::uint32_t BoxedParts = 0x3333F;
inline std::uint32_t OpenParts(const std::uint8_t* script, int frame, std::uint32_t to) {
    if (!script) return AllParts;
    const auto u16 = [](const std::uint8_t* at) { return static_cast<unsigned>(at[0] | at[1] << 8); };
    const auto u32 = [](const std::uint8_t* at) { std::uint32_t value; std::memcpy(&value, at, 4); return value; };
    bool said = false;
    std::uint32_t parts = 0;
    // ponytail: a live script is the game's own, so its counts are taken as
    // they are, up to a size no stock script comes near.
    const unsigned lists = (std::min)(u16(script + 0x12), 32u);
    for (unsigned j = 0; j < lists; ++j) {
        const std::uint8_t* list = script + 0x18 + j * 12;
        if (u16(list) != 8) continue;
        const unsigned commands = (std::min)(u16(list + 2), 64u);
        const std::uint8_t* frames = list + u32(list + 4), * data = list + u32(list + 8);
        for (unsigned k = 0; k < commands; ++k) {
            if (frame < static_cast<int>(u16(frames + k * 4)) || frame >= static_cast<int>(u16(frames + k * 4 + 2))) continue;
            if (!(u32(data + k * 16) & to)) continue;
            said = true; parts |= u32(data + k * 16 + 4);
        }
    }
    return said ? parts : AllParts;
}

// One box of the game: its four corners, 16 bytes each (x, y, z and one more
// float). Reads the 0x40 bytes at record. False for a box without area or
// with a number that is not one, which is how an unused record reads.
inline bool ReadBox(const std::uint8_t* record, BoxKind kind, Box& box) {
    for (int corner = 0; corner < 4; ++corner) {
        float at[2];
        std::memcpy(at, record + corner * 0x10, sizeof(at));
        if (!std::isfinite(at[0]) || !std::isfinite(at[1])) return false;
        box.x[corner] = at[0]; box.y[corner] = at[1];
    }
    // Twice the area, by the corners' cross products.
    float area = 0;
    for (int corner = 0; corner < 4; ++corner) area += box.x[corner] * box.y[(corner + 1) % 4] - box.x[(corner + 1) % 4] * box.y[corner];
    if (std::fabs(area) < .001f) return false;
    box.kind = kind; box.side = 0;
    return true;
}

// A point of the fight plane on a screen of the given size: a point is
// multiplied from the left, as Direct3D does. False when it is behind the camera.
inline bool Project(const float* view, const float* projection, float x, float y, float width, float height, float& screenX, float& screenY) {
    const float in[4] = {x, y, 0, 1};
    float eye[4], clip[4];
    for (int column = 0; column < 4; ++column) eye[column] = in[0] * view[column] + in[1] * view[4 + column] + in[2] * view[8 + column] + in[3] * view[12 + column];
    for (int column = 0; column < 4; ++column) clip[column] = eye[0] * projection[column] + eye[1] * projection[4 + column] + eye[2] * projection[8 + column] + eye[3] * projection[12 + column];
    if (clip[3] <= 1e-4f) return false;
    screenX = (clip[0] / clip[3] * .5f + .5f) * width;
    screenY = (.5f - clip[1] / clip[3] * .5f) * height;
    return true;
}
} }
