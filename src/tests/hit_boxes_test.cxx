#include "../training/HitBoxes.hxx"
#include "test_support.hxx"

using namespace sf4e::training;

// An upright box from its edges.
static Box Upright(float x0, float y0, float x1, float y1, BoxKind kind, int side) {
    return Box{{x0, x1, x1, x0}, {y1, y1, y0, y0}, kind, static_cast<std::uint8_t>(side)};
}

int main() {
    // A box the way the game lays one out: top left, top right, bottom right, bottom left.
    const float corners[16] = {1, 1.2f, 0, 1, 1.6f, 1.2f, 0, 1, 1.6f, .5f, 0, 1, 1, .5f, 0, 1};
    std::uint8_t record[0x40];
    std::memcpy(record, corners, sizeof(record));
    Box box{};
    CHECK(ReadBox(record, BoxKind::Hit, box));
    CHECK(box.x[0] == 1 && box.x[1] == 1.6f && box.y[0] == 1.2f && box.y[2] == .5f && box.kind == BoxKind::Hit);
    // An unused record is all zeros; one that was never written can hold anything.
    std::uint8_t empty[0x40] = {};
    CHECK(!ReadBox(empty, BoxKind::Hurt, box));
    const float infinite = INFINITY;
    std::memcpy(record + 0x14, &infinite, 4);
    CHECK(!ReadBox(record, BoxKind::Hurt, box));

    // The matrices of a training fight as the game held them: Player 1 stands
    // at x = -1.5, left of the middle and near the bottom of a 1920x1080 screen.
    const float view[16] = {1, 0, 0, 0, 0, .998146f, .0608626f, 0, 0, -.0608626f, .998146f, 0, 0, -1.27887f, 4.02963f, 1};
    const float projection[16] = {1.5454f, 0, 0, 0, 0, 2.74739f, 0, 0, 0, 0, 1.0001f, 1, 0, 0, -.10001f, 0};
    float sx = 0, sy = 0;
    CHECK(Project(view, projection, -1.5f, 0, 1920, 1080, sx, sy));
    CHECK(sx > 407 && sx < 409 && sy > 1010 && sy < 1012);
    CHECK(Project(view, projection, 0, 1.27887f / .998146f, 1920, 1080, sx, sy));
    CHECK(sx == 960 && sy > 539 && sy < 541);
    const float behind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1};
    CHECK(!Project(behind, projection, 0, 0, 1920, 1080, sx, sy));
    // A strike touches the other fighter's hurt box only, a throw its body box only.
    const Box hurt = Upright(1, 0, 2, 1, BoxKind::Hurt, 1), bodyBox = Upright(1, 0, 2, 1, BoxKind::Body, 1);
    const Box strike = Upright(.5f, .2f, 1.1f, .4f, BoxKind::Hit, 0), grab = Upright(.5f, .2f, 1.1f, .4f, BoxKind::Throw, 0);
    CHECK(Touches({hurt, strike}, BoxKind::Hit) && !Touches({bodyBox, strike}, BoxKind::Hit));
    CHECK(Touches({bodyBox, grab}, BoxKind::Throw) && !Touches({hurt, grab}, BoxKind::Throw));
    // How far in, or how far apart.
    const float in = TouchDepth({hurt, strike}, BoxKind::Hit), apart = TouchDepth({Upright(1.3f, 0, 2, 1, BoxKind::Hurt, 1), strike}, BoxKind::Hit);
    CHECK(in > .09f && in < .11f && apart < -.19f && apart > -.21f);
    CHECK(TouchDepth({hurt}, BoxKind::Hit) == -1000);
    // Asked about one side, the other side's attack does not count.
    CHECK(TouchDepth({hurt, strike}, BoxKind::Hit, 0) == in && TouchDepth({hurt, strike}, BoxKind::Hit, 1) == -1000);
    // Its own boxes do not count, and neither do boxes that only share an edge.
    const Box own = Upright(1, 0, 2, 1, BoxKind::Hurt, 0), edge = Upright(1.1f, 0, 2, 1, BoxKind::Hurt, 1);
    CHECK(!Touches({own, strike}, BoxKind::Hit) && !Touches({edge, strike}, BoxKind::Hit));
    // A box standing on a corner (a square turned an eighth) does not reach
    // into the corner of the upright rectangle round it: a strike there misses.
    const Box turned{{1.5f, 2, 1.5f, 1}, {1, .5f, 0, .5f}, BoxKind::Hurt, 1};
    CHECK(!Touches({turned, Upright(.9f, .8f, 1.15f, .95f, BoxKind::Hit, 0)}, BoxKind::Hit));
    CHECK(Touches({turned, Upright(.9f, .4f, 1.15f, .6f, BoxKind::Hit, 0)}, BoxKind::Hit));
    // A script with one list of commands of kind 8: frames 0 to 12 open to
    // nothing (strike and throw named, no parts), frames 12 to 24 open to a
    // strike on waist, stomach and head. Before the list, the header's 0x18 bytes.
    std::uint8_t script[0x18 + 12 + 8 + 32] = {};
    const auto put16 = [&](std::size_t at, unsigned value) { script[at] = static_cast<std::uint8_t>(value); script[at + 1] = static_cast<std::uint8_t>(value >> 8); };
    const auto put32 = [&](std::size_t at, std::uint32_t value) { std::memcpy(script + at, &value, 4); };
    put16(0x12, 1);
    put16(0x18, 8); put16(0x1A, 2); put32(0x1C, 12); put32(0x20, 20);
    put16(0x24, 0); put16(0x26, 12); put16(0x28, 12); put16(0x2A, 24);
    put32(0x2C, OpenToStrike | OpenToThrow); put32(0x30, 0);
    put32(0x3C, OpenToStrike); put32(0x40, 0xB);
    CHECK(OpenParts(script, 0, OpenToStrike) == 0 && OpenParts(script, 11, OpenToThrow) == 0);
    CHECK(OpenParts(script, 12, OpenToStrike) == 0xB && OpenParts(script, 23, OpenToStrike) == 0xB);
    // Nothing says anything about throws from frame 12 on, or about anything from 24 on, or without a script.
    CHECK(OpenParts(script, 12, OpenToThrow) == AllParts && OpenParts(script, 24, OpenToStrike) == AllParts);
    CHECK(OpenParts(nullptr, 0, OpenToStrike) == AllParts);
    // Another kind of list says nothing.
    put16(0x18, 9);
    CHECK(OpenParts(script, 0, OpenToStrike) == AllParts);
    return 0;
}
