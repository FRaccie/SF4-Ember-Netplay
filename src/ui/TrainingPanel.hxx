#pragma once
#include "../training/TrainingSession.hxx"
#include "../training/MatchPractice.hxx"
#include <functional>
#include <string>
#include "MenuNavigation.hxx"

namespace sf4e { namespace ui {
using TrainingSubmit = std::function<bool(training::Command)>;
void DrawTrainingPanel(const training::View& view, const TrainingSubmit& submit);
// Shared by the live overlay and render tests; owns fixed geometry and input focus.
void DrawTrainingFlyout(const training::View& view, const TrainingSubmit& submit);
void ShowTrainingRecordings();
// Where the lab keeps the dummy's reply and the hotkeys (training.json) and
// the saved recordings. Until this is set they live in memory only, which is
// what the render tests want.
void SetTrainingDirectory(std::wstring directory);
// Reset position and save position on keys the player picks (F2 and F11 at
// first), over the fight or the controls. Also sends the dummy's plan each battle.
// padSelect: the pad's Select button is down; a tap resets the position and a hold saves it.
void TrainingHotkeys(const training::View& view,const TrainingSubmit& submit,bool padSelect=false);
// Whether a hotkey sits on the key that many after F1, so the game is not given it.
bool TrainingHotkeyBound(int fromF1);
// The two hotkeys as the HUD's hint names them, those without a key left out.
std::string TrainingKeyHints();
// What the last position command did, while it is news; empty after a few seconds.
std::string TrainingNotice(bool& failed);
// Every part of the HUD is the player's to turn off (F6, HUD), kept in
// training.json; F5 hides the whole HUD. readings: the frame data line.
// inputs: the pressed buttons. counts: each run's length on the bars. state:
// what the meter is doing. shortcuts: the chips. hints: the keys named beside
// them. status: the strip a recording or a playback adds.
struct TrainingHudParts { bool readings = true, inputs = true, bars = true, counts = true, legend = true, state = true, shortcuts = true, hints = true, status = true; };
// One row a part on the HUD page, and its name in training.json.
struct TrainingHudPart { const char* id; const char* label; bool TrainingHudParts::* shown; };
constexpr TrainingHudPart TrainingHudPartList[] = {
    {"readings", "training.hud.readings", &TrainingHudParts::readings}, {"inputs", "training.hud.inputs", &TrainingHudParts::inputs},
    {"bars", "training.hud.bars", &TrainingHudParts::bars}, {"counts", "training.hud.counts", &TrainingHudParts::counts},
    {"legend", "training.hud.legend", &TrainingHudParts::legend}, {"state", "training.hud.state", &TrainingHudParts::state},
    {"shortcuts", "training.hud.shortcuts", &TrainingHudParts::shortcuts}, {"hints", "training.hud.hints", &TrainingHudParts::hints},
    {"status", "training.hud.status", &TrainingHudParts::status}};
const TrainingHudParts& TrainingHudShown();
MenuNavigation& TrainingNavigation();
// Passive, except for its chips: open the controls, replay the selected slot,
// stop playback. pointer: the pointer is over the chips, so the overlay takes
// the mouse (only) from the game.
struct TrainingHudInput { bool open = false, replay = false, stop = false, pointer = false; };
// Bottom edge of the frame meter as a fraction of the viewport height, above
// the game's super meters.
constexpr float TrainingHudBottom = .82f;
TrainingHudInput DrawTrainingHud(const training::View& view);
// The frame meter alone, over a rollback match the runtime is watching.
void DrawMatchMeter(const training::View& view);
// In a match under a table's Training rule: draws the shared position controls'
// unavailable message. Returns zero; padSelect does not request a command.
unsigned MatchPracticeKeys(bool padSelect);
} }
