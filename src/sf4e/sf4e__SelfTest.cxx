#include "sf4e__SelfTest.hxx"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../platform/Utf8.hxx"
#include "../training/HitBoxes.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__NetplayFacade.hxx"

// The training test: the frame meter against moves whose frame data is known,
// typed in numpad notation and played by the lab, a recording and its
// playback, and the lab's saved position. An offline Versus battle of Ryu
// against Ryu is entered the way a room's match is, with no menu, and the lab
// is let into that battle. It uses only what the lab's own controls send
// (training::Submit) and reads.
//
// The room test: two PCs play one game of a private room through the
// runtime's commands, the ones the room's controls send. room-host opens the
// room and writes its invitation to the file SF4E_SELFTEST_INVITE names; the
// runner copies it to the other PC, where room-guest joins with it. Each sits,
// readies as Ryu and plays until the match is live; the host then ends the
// game and both leave.
//
// The hitboxes test: from the same battle, Player 1 attacks Player 2 with the
// moves of kProbes and every try has to come out as the lab's boxes show it.
namespace sf4e { namespace selftest {
namespace {

using training::Action;
using training::Mode;

// Ticks are the runtime's, sixty a second.
constexpr int kSecond = 60;
// Frames the test records before it stops the recording.
constexpr int kRecorded = 8 * kSecond;
enum class Stage { Off, ToMainMenu, ToBattle, Move, Starting, Settling, Measure, Record, Standby, Recording, Replaying, SavePosition, Dash, Dashed, Reset, BoxSave, BoxNext, BoxPlace, BoxPlay, BoxRun, RoomSetup, RoomJoin, RoomOpening, RoomSeat, RoomReady, RoomPlaying, RoomEnding, RoomLeaving, Closing };

// Ryu's crouching LP: startup 3, active 2, recovery 7. The numbers on the
// meter's line have to be these. Its cells follow the startup number
// (FrameMeter.hxx: ClassifyMeter), so the recovery has one cell less.
constexpr int kStartup = 3, kActive = 2, kRecovery = 7;

struct Run {
	Stage stage = Stage::Off;
	// Where a played move goes once it has ended, and where it goes if it does not.
	Stage then = Stage::Off, otherwise = Stage::Off;
	std::string playing;
	bool failed = false;
	// Ticks in this stage, and the frames its condition has held in a row.
	int waited = 0, held = 0;
	// Frames recorded, and the furthest frame the playback was seen at.
	int recorded = 0, reached = 0;
	float saved = 0, dashed = 0;
	std::string screen;
	std::wstring resultFile, inviteFile;
	std::vector<std::string> lines;
	// Who fights, as the game numbers them (0 Ryu, 1 Ken): SF4E_SELFTEST_FIGHTERS="1,0".
	int fighters[2] = {0, 0};
	// The hitboxes test: which probe, which try of it, the fighters' distance
	// in this try, the nearest whiff and the farthest hit so far.
	std::size_t at = 0;
	int trial = 0, wrong = 0;
	float distance = 0, hits = 0, whiffs = 0, vitality = 0, own = 0;
	bool hit = false, played = false;
	// How far the lab's own boxes showed the attack into the opponent in this try, at the most.
	float depth = -1000;
	// SF4E_SELFTEST_ONLY: only the probes whose name holds this are played.
	std::string only;
	std::ofstream dump;
} s_run;

std::wstring Environment(const wchar_t* name) {
	const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
	if (!size) return {};
	std::wstring value(size, L'\0');
	value.resize(GetEnvironmentVariableW(name, value.data(), size));
	return value;
}

void Say(bool pass, const std::string& step, const std::string& detail = {}) {
	const std::string line = std::string(pass ? "PASS " : "FAIL ") + step + (detail.empty() ? "" : ": " + detail);
	if (pass) spdlog::info("SelfTest: {}", line); else spdlog::error("SelfTest: {}", line);
	s_run.lines.push_back(line);
	if (!pass) s_run.failed = true;
}

void Enter(Stage stage) { s_run.stage = stage; s_run.waited = 0; s_run.held = 0; }
bool Late(int seconds) { return s_run.waited > seconds * kSecond; }

// The outcome is written, and the game asked to close as its updater asks it.
void Finish() {
	if (s_run.dump.is_open()) s_run.dump.close();
	s_run.lines.push_back(s_run.failed ? "RESULT fail" : "RESULT pass");
	spdlog::info("SelfTest: {}", s_run.lines.back());
	std::ofstream out(s_run.resultFile, std::ios::trunc);
	for (const std::string& line : s_run.lines) out << line << '\n';
	out.flush();
	if (!out) spdlog::error("SelfTest: the result could not be written to '{}'", platform::WideToUtf8(s_run.resultFile));
	Dimps::Platform::Main::Close();
	Enter(Stage::Closing);
}

// A failed step goes on at `otherwise`, or ends the run when that is Closing.
void Fail(const std::string& step, const std::string& detail, Stage otherwise) {
	Say(false, step, detail);
	if (otherwise == Stage::Closing) Finish(); else Enter(otherwise);
}

// The two fighters on the first stage: Ryu against Ryu unless the hitboxes test was given others.
void SetFighters() {
	Dimps::GameEvents::VsMode::ConfirmedCharaConditions sides[2]{};
	for (int side = 0; side < 2; side++) { sides[side].charaID = s_run.fighters[side]; sides[side].unc_edition = Dimps::Game::Battle::ED_USF4; }
	if (!GameEvents::VsPreBattle::SetSides(sides, 0)) spdlog::error("SelfTest: VS mode is not in front; the fighters were not set");
}

// False when the main menu is not the one in front.
bool EnterBattle() {
	GameEvents::VsPreBattle::VersusSettings settings;
	settings.editionSelect = TRUE;
	// The longest round the game offers: the test is done long before it.
	settings.timeLimit = { 0, 9999 };
	// No replay is saved, however the battle ends: the test writes nothing to the saves.
	settings.recordReplay = FALSE;
	GameEvents::VsBattle::nextRequest = {};
	// The lab runs in a Training battle only; a test run lets it into this one.
	training::AllowOfflineVersusForTest();
	return GameEvents::VsPreBattle::GoToVersus(settings, SetFighters);
}

// loop: for Action::Loop, whether playback repeats.
bool Submit(Action action, std::vector<training::Input> frames = {}, bool loop = false) {
	training::Command command;
	command.action = action; command.generation = training::ReadView().generation;
	command.frames = std::move(frames); command.side = 0; command.loop = loop;
	return training::Submit(std::move(command));
}

// Player 1 plays the typed moves through the selected slot, facing right.
// A test run is a game of its own, so that slot is the only one with
// anything in it and Play plays it. Once the playback has begun, ended, and
// the fighter has stood for half a second, the test goes on at `then`.
void Play(const char* moves, const std::string& what, Stage then, Stage otherwise) {
	s_run.playing = what; s_run.then = then; s_run.otherwise = otherwise;
	std::vector<std::string> steps;
	combo::Compiled compiled;
	std::string error;
	if (!combo::ParseSteps(moves, steps, error) || !combo::Synthesize(steps, true, compiled, error)) { Fail(what, std::string("'") + moves + "' is not moves: " + error, otherwise); return; }
	if (!Submit(Action::ClearHistory) || !Submit(Action::Load, std::move(compiled.frames)) || !Submit(Action::Play)) { Fail(what, "the lab did not take the moves", otherwise); return; }
	Enter(Stage::Starting);
}

void Measure(const training::View& view) {
	const Cells cells = CountCells(view.meter, 0);
	const auto& move = view.meter.moves[0];
	const bool numbersRight = view.meter.startupFrames[0] == kStartup && move.active == kActive && move.recovery == kRecovery;
	const bool cellsRight = cells.startup == kStartup && cells.active == kActive && cells.recovery == kRecovery - 1;
	Say(numbersRight, "Ryu crouching LP numbers", "startup " + std::to_string(view.meter.startupFrames[0]) + ", active " +
		std::to_string(move.active) + ", recovery " + std::to_string(move.recovery) + (numbersRight ? "" : ", expected 3/2/7"));
	Say(cellsRight, "Ryu crouching LP cells", std::to_string(cells.startup) + ", " + std::to_string(cells.active) + " and " +
		std::to_string(cells.recovery) + " cells" + (cellsRight ? "" : ", expected 3, 2 and 6"));
}

// The hitboxes test (hitboxes) asks the game itself where a
// fighter can be hit: Player 1 attacks a standing Player 2 from a distance
// that is halved towards the last one that hit, so the attack's reach is
// known to about a hundredth of a unit, and every frame of every try is
// written raw to <result file>.boxes: the fighters' state, the start of both
// actors, all their body boxes and the boxes of their attacks. Which of the
// game's boxes are the ones that count is then read off that file
// (scripts/analyze-boxes.py), not guessed. Each try also has to come out as
// the lab's own boxes show it: touching when it hit, apart when it did not.
// sweep: the reach is searched. Otherwise the move is played once from that
// distance. other: what Player 2 does meanwhile: nothing, "2" to crouch, or
// a move of its own. delay: frames Player 1 waits before its move, so that it
// lands on a crouch already taken or on a limb still held out; -1 lands it on
// the other move's first frames. judged: the lab's boxes have to tell the outcome.
// explore: the dump also takes memory to search for what an actor's boxes do
// not say: 1 both actors whole (what makes a limb one that can be hit), 2
// what the effect unit points to (where a fireball is kept).
struct Probe { const char* moves; const char* what; bool sweep; float from; const char* other; int delay; bool judged; int explore = 0; };
const Probe kProbes[] = {
	{"5LP", "standing LP", true, 0.7f, "", 0, true},
	{"5MP", "standing MP", true, 0.7f, "", 0, true},
	{"5HP", "standing HP", true, 0.7f, "", 0, true},
	{"5LK", "standing LK", true, 0.7f, "", 0, true},
	{"5MK", "standing MK", true, 0.7f, "", 0, true},
	{"5HK", "standing HK", true, 0.7f, "", 0, true},
	{"2LP", "crouching LP", true, 0.7f, "", 0, true},
	{"2MP", "crouching MP", true, 0.7f, "", 0, true},
	{"2HP", "crouching HP", true, 0.7f, "", 0, true},
	{"2LK", "crouching LK", true, 0.7f, "", 0, true},
	{"2MK", "crouching MK", true, 0.7f, "", 0, true},
	{"2HK", "crouching HK", true, 0.7f, "", 0, true},
	{"j.HK", "jumping HK", true, 1.8f, "", 0, true},
	{"623HP", "HP Shoryuken", true, 0.7f, "", 0, true},
	{"LP+LK", "throw", true, 0.7f, "", 0, true},
	// The same on an opponent that crouches: its boxes are other ones.
	{"5MK", "standing MK on a crouch", true, 0.7f, "2", 20, true},
	{"2MK", "crouching MK on a crouch", true, 0.7f, "2", 20, true},
	{"2HP", "crouching HP on a crouch", true, 0.7f, "2", 20, true},
	{"LP+LK", "throw on a crouch", true, 0.7f, "2", 20, true},
	// On an opponent whose own attack is out, from further than that attack
	// reaches: a reach beyond the one on a standing opponent is the limb it holds out.
	{"5MK", "standing MK on a standing HK", true, 2.2f, "5HK", 12, true, 1},
	// A fireball's box travels: once, from across the screen.
	{"236LP", "LP Hadoken", false, 3.f, "", 0, true, 2},
	// On frames that cannot be hit: the first of a Shoryuken and of a backdash.
	{"5LP", "standing LP on a Shoryuken", false, 0.9f, "623HP", -1, true},
	{"5LP", "standing LP on a backdash", false, 0.9f, "44", -1, true},
};
constexpr float kFar = 4.f;
constexpr int kHalvings = 8;
constexpr float kHair = 0.01f;

// A read of the game's memory that fails instead of faulting.
bool Peek(std::uint32_t at, void* out, std::size_t size) {
	SIZE_T read = 0;
	return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, size, &read) && read == size;
}
template <class T> void Put(std::ofstream& out, const T& value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }

// One frame: a header, then for each fighter its state, the first 0x200 bytes
// of the actor, its body boxes, and its six attack lists (the word at the
// actor, the count, how many boxes follow, then the boxes, 0xC0 bytes each).
// Last, a count of explored blocks, none for most probes.
void DumpFrame() {
	namespace rBattle = Dimps::Game::Battle;
	using Actor = rBattle::Chara::Actor;
	Run& run = s_run;
	rBattle::System* const system = rBattle::System::staticMethods.GetSingleton();
	rBattle::Chara::Unit* const unit = system ? (system->*rBattle::System::publicMethods.GetCharaUnit)() : nullptr;
	if (!unit || !run.dump) return;
	for (unsigned side = 0; side < 2; side++) if (!(unit->*rBattle::Chara::Unit::publicMethods.GetActorByIndex)(side)) return;
	Put(run.dump, std::uint32_t(0x46584F42)); Put(run.dump, std::uint32_t(run.at)); Put(run.dump, std::uint32_t(run.trial));
	Put(run.dump, run.distance); Put(run.dump, std::uint32_t(run.waited));
	Put(run.dump, std::int32_t(rBattle::System::GetNumFramesSimulated_FixedPoint(system)->integral));
	for (unsigned side = 0; side < 2; side++) {
		Actor* const actor = (unit->*rBattle::Chara::Unit::publicMethods.GetActorByIndex)(side);
		const unsigned char* const bytes = reinterpret_cast<const unsigned char*>(actor);
		const float* const root = (actor->*Actor::publicMethods.GetCurrentRootPosition)();
		Dimps::Math::FixedPoint frame{}, vitality{};
		(actor->*Actor::publicMethods.GetActionFrame)(&frame);
		(actor->*Actor::publicMethods.GetVitalityAmt_FixedPoint)(&vitality);
		run.dump.write(reinterpret_cast<const char*>(root), 12);
		Put(run.dump, std::uint32_t((actor->*Actor::publicMethods.GetStatus)())); Put(run.dump, std::int32_t((actor->*Actor::publicMethods.GetActionID)()));
		Put(run.dump, Dimps::Math::FixedToFloat(&frame)); Put(run.dump, Dimps::Math::FixedToFloat(&vitality));
		run.dump.write(reinterpret_cast<const char*>(bytes), 0x200);
		run.dump.write(reinterpret_cast<const char*>(Actor::GetBodyBox(actor, 0)), 120 * 0xA0);
		// Six words from +0x130 that may each point to a list of attack boxes.
		// Not every one does in every state (one read 1 in an offline Versus
		// battle), so they are followed with reads that can fail.
		for (unsigned list = 0; list < 6; list++) {
			std::uint32_t head = 0, count = 0, record = 0, kept = 0;
			std::memcpy(&head, bytes + 0x130 + list * 4, 4);
			const bool listed = Peek(head + 0x2C, &count, 4) && Peek(head + 0x20, &record, 4);
			char records[16][0xC0];
			for (; listed && kept < 16 && kept < count && Peek(record, records[kept], 0xC0); kept++) std::memcpy(&record, records[kept] + Actor::BOX_NEXT, 4);
			Put(run.dump, head); Put(run.dump, count); Put(run.dump, kept);
			run.dump.write(records[0], kept * 0xC0);
		}
	}
	// For a probe that explores: how many blocks, then
	// for each its address, the block that pointed to it and where, and 0x400 bytes.
	struct Node { std::uint32_t at, from, where; int depth; };
	std::vector<Node> nodes;
	if (kProbes[run.at].explore == 1) {
		// Both actors whole, in blocks that are not followed: what says that
		// a limb can be hit just now is somewhere in them.
		for (unsigned side = 0; side < 2; side++)
			for (std::uint32_t at = 0; at < 0x7000; at += 0x400)
				nodes.push_back({reinterpret_cast<std::uint32_t>((unit->*rBattle::Chara::Unit::publicMethods.GetActorByIndex)(side)) + at, 0xFFFFFFF0 + side, at, 4});
	}
	// A fireball is an effect, not a fighter: what the effect unit points to, four steps deep.
	if (kProbes[run.at].explore == 2)
		nodes.push_back({reinterpret_cast<std::uint32_t>((system->*rBattle::System::publicMethods.GetUnitByIndex)(rBattle::System::U_EFFECT)), 0, 0, 0});
	std::vector<char> blocks;
	for (std::size_t n = 0; n < nodes.size(); n++) {
		char block[0x400] = {};
		// A block that ends before 0x400 bytes is read as far as its page goes.
		if (!Peek(nodes[n].at, block, sizeof(block)) && !Peek(nodes[n].at, block, 0x1000 - (nodes[n].at & 0xFFF) < sizeof(block) ? 0x1000 - (nodes[n].at & 0xFFF) : sizeof(block))) { nodes.erase(nodes.begin() + n--); continue; }
		blocks.insert(blocks.end(), block, block + sizeof(block));
		// From 4 on: the first word is the object's table of functions, in the game's own image, like every address below the heap.
		for (std::uint32_t where = 4; nodes[n].depth < 4 && where < 0x400 && nodes.size() < 1500; where += 4) {
			std::uint32_t next = 0; char probe = 0;
			std::memcpy(&next, block + where, 4);
			bool seen = next < 0x01000000 || (next & 3);
			for (const Node& node : nodes) seen = seen || node.at == next;
			if (!seen && Peek(next, &probe, 1)) nodes.push_back({next, nodes[n].at, where, nodes[n].depth + 1});
		}
	}
	Put(run.dump, std::uint32_t(nodes.size()));
	for (std::size_t n = 0; n < nodes.size(); n++) { Put(run.dump, nodes[n].at); Put(run.dump, nodes[n].from); Put(run.dump, nodes[n].where); run.dump.write(blocks.data() + n * 0x400, 0x400); }
}

float Vitality(int side) {
	namespace rBattle = Dimps::Game::Battle;
	rBattle::System* const system = rBattle::System::staticMethods.GetSingleton();
	rBattle::Chara::Unit* const unit = system ? (system->*rBattle::System::publicMethods.GetCharaUnit)() : nullptr;
	rBattle::Chara::Actor* const actor = unit ? (unit->*rBattle::Chara::Unit::publicMethods.GetActorByIndex)(side) : nullptr;
	if (!actor) return 0;
	Dimps::Math::FixedPoint vitality{};
	(actor->*rBattle::Chara::Actor::publicMethods.GetVitalityAmt_FixedPoint)(&vitality);
	return Dimps::Math::FixedToFloat(&vitality);
}

std::string Number(float value) { char text[32]; snprintf(text, sizeof(text), "%.3f", value); return text; }

// Player 1 plays a probe's moves from the selected slot, facing right, while
// Player 2 does the probe's other moves through training::PlayTrackForTest.
bool PlayProbe(const Probe& probe) {
	std::vector<std::string> steps, otherSteps;
	combo::Compiled compiled, other;
	std::string error;
	if (!combo::ParseSteps(probe.moves, steps, error) || !combo::Synthesize(steps, true, compiled, error)) return false;
	std::vector<training::Input>& frames = compiled.frames;
	std::vector<training::Input> track;
	// A jumping attack is laid out as a jump straight up and its button
	// twelve frames later, on the way up, where it passes over a standing
	// opponent. Here it is a jump towards the opponent with the button
	// fourteen frames later still, on the way down: a jump-in.
	if ((probe.moves[0] == 'j' || probe.moves[0] == 'J') && probe.moves[1] == '.' && frames.size() > 12) {
		for (int i = 0; i < 2; i++) frames[i].mapped = frames[i].raw = combo::DirectionBits('9', true);
		frames.insert(frames.begin() + 12, 14, training::Input{});
	}
	if (std::string(probe.other) == "2") track.assign(300, {combo::DirectionBits('2', false), combo::DirectionBits('2', false), 0, 0});
	else if (*probe.other) {
		if (!combo::ParseSteps(probe.other, otherSteps, error) || !combo::Synthesize(otherSteps, false, other, error)) return false;
		track = std::move(other.frames);
	}
	// A light attack is out three frames after its press.
	int delay = probe.delay;
	if (delay < 0) delay = static_cast<int>(track.size()) > 2 ? static_cast<int>(track.size()) - 2 : 0;
	frames.insert(frames.begin(), static_cast<std::size_t>(delay), training::Input{});
	training::PlayTrackForTest(1, std::move(track));
	return Submit(Action::ClearHistory) && Submit(Action::Load, std::move(frames)) && Submit(Action::Play);
}

void Note(const std::string& line) { spdlog::info("SelfTest: {}", line); s_run.lines.push_back("INFO " + line); }

// The stages of the hitboxes test.
void TickBoxes(const training::View& view, bool neutral) {
	Run& run = s_run;
	const bool otherNeutral = view.meter.current[1].valid && training::ClassifyStatus(view.meter.current[1].status) == training::Phase::Neutral;
	switch (run.stage) {
	case Stage::BoxSave:
		// The saved position gives every try the same health and clock back.
		if (run.waited == 1 && !Submit(Action::Save)) { Fail("save position", "the lab did not take the command", Stage::Closing); break; }
		if (view.checkpoint) { run.hits = kProbes[0].from; run.whiffs = kFar; Enter(Stage::BoxNext); }
		else if (Late(5)) Fail("save position", "nothing was saved", Stage::Closing);
		break;
	case Stage::BoxNext: {
		while (run.at < std::size(kProbes) && !run.trial && !run.only.empty() && std::string(kProbes[run.at].what).find(run.only) == std::string::npos) {
			if (++run.at < std::size(kProbes)) { run.hits = kProbes[run.at].from; run.whiffs = kFar; }
		}
		if (run.at == std::size(kProbes)) { Finish(); break; }
		const Probe& probe = kProbes[run.at];
		run.distance = probe.sweep && run.trial ? (run.hits + run.whiffs) / 2 : probe.from;
		if (Submit(Action::Restore)) Enter(Stage::BoxPlace); else Fail(probe.what, "the lab did not put the position back", Stage::Closing);
		break;
	}
	case Stage::BoxPlace: {
		if (run.waited < 10) break;
		training::Command command;
		command.action = Action::Place; command.generation = view.generation;
		command.place[0] = -run.distance / 2; command.place[1] = run.distance / 2;
		if (training::Submit(std::move(command))) Enter(Stage::BoxPlay); else Fail(kProbes[run.at].what, "the lab did not place the fighters", Stage::Closing);
		break;
	}
	case Stage::BoxPlay:
		if (run.waited < 20) break;
		run.vitality = Vitality(1); run.own = Vitality(0); run.hit = run.played = false; run.depth = -1000;
		if (PlayProbe(kProbes[run.at])) Enter(Stage::BoxRun); else Fail(kProbes[run.at].what, "the lab did not take the moves", Stage::Closing);
		break;
	case Stage::BoxRun: {
		const Probe& probe = kProbes[run.at];
		DumpFrame();
		run.played = run.played || view.mode == Mode::Playback;
		const unsigned status = view.meter.current[1].valid ? view.meter.current[1].status : 0;
		run.hit = run.hit || (status >= 21 && status <= 24) || Vitality(1) < run.vitality;
		// What the player is shown: a red box on a green one, or a blue one on a yellow one.
		run.depth = (std::max)(run.depth, (std::max)(training::TouchDepth(view.boxes.boxes, training::BoxKind::Hit, 0), training::TouchDepth(view.boxes.boxes, training::BoxKind::Throw, 0)));
		// Played out, and both fighters standing again for a third of a second.
		if (run.played && view.mode == Mode::Idle && neutral && otherNeutral) run.held++; else run.held = 0;
		if (run.held < kSecond / 3 && !Late(15)) break;
		training::PlayTrackForTest(1, {});
		Note(std::string(probe.what) + " (" + probe.moves + ") try " + std::to_string(run.trial) + " at " + Number(run.distance) + ": " + (run.hit ? "hit" : "whiff") +
			", boxes " + (run.depth > -1000 ? Number(run.depth) : "none") + (Vitality(0) < run.own ? ", Player 1 was hit" : "") + (run.played ? "" : ", the playback never started"));
		// The boxes have to tell the outcome: touching in every try that hit, apart in every one that did not.
		// A try decided by less than a hundredth of a unit, a fraction of a pixel, is
		// nearer than the search itself goes and counts for neither.
		if (probe.judged && (run.depth > 0) != run.hit && (run.depth > kHair || run.depth < -kHair)) run.wrong++;
		if (probe.sweep) {
			if (!run.trial && !run.hit) { Note(std::string(probe.what) + ": did not hit from " + Number(probe.from) + ", so its reach was not searched"); run.trial = kHalvings; }
			else if (run.trial) (run.hit ? run.hits : run.whiffs) = run.distance;
		}
		if (probe.sweep && ++run.trial <= kHalvings) { Enter(Stage::BoxNext); break; }
		if (probe.sweep && run.hits > probe.from) Note(std::string(probe.what) + " reach: hits from " + Number(run.hits) + ", whiffs from " + Number(run.whiffs));
		if (probe.judged) Say(!run.wrong, std::string(probe.what) + " boxes", run.wrong ? std::to_string(run.wrong) + " tries where the boxes and the outcome disagree" : "touch in every try that hit and in none that did not");
		run.wrong = 0;
		run.at++; run.trial = 0;
		if (run.at < std::size(kProbes)) { run.hits = kProbes[run.at].from; run.whiffs = kFar; }
		Enter(Stage::BoxNext);
		break;
	}
	default:
		break;
	}
}

// Every change of the screen in front, arrival and departure alike, goes to
// the log: the script sends its one Enter only while the newest line names the
// title screen.
void WatchScreen(const std::string& screen) {
	if (screen == s_run.screen) return;
	s_run.screen = screen;
	spdlog::info("SelfTest: screen {}", screen.empty() ? "none" : screen);
}

// The room test's side of the runtime: its snapshot, and commands as the
// room's controls build them, tagged with the current generation so a stale
// one is dropped as a stale press would be.
using NetplayFacade::RuntimeCommand;
using netplay::CommandKind;
bool Host() { return Requested() == Test::RoomHost; }
std::shared_ptr<const NetplayFacade::RuntimeSnapshot> Now() { return NetplayFacade::GetRuntimeSnapshotShared(); }
RuntimeCommand Command(CommandKind kind) {
	const auto now = Now();
	RuntimeCommand request;
	request.command = { kind, now->session.generation, {} };
	request.preferences = now->preferences;
	request.displayName = now->preferences.displayName;
	return request;
}
RuntimeCommand Room(room::ActionKind kind) {
	RuntimeCommand request = Command(CommandKind::RoomAction);
	const auto& room = Now()->room;
	request.roomAction.kind = kind; request.roomAction.table = 0;
	request.roomAction.roomEpoch = room.roomEpoch; request.roomAction.revision = room.revision;
	request.roomAction.tableRevision = room.tables[0].revision;
	// Ending a game names the game it ends, as the room's own controls do.
	if (kind == room::ActionKind::AbortMatch) request.roomAction.matchGeneration = room.tables[0].matchGeneration;
	return request;
}
// A waiting step sends again every few seconds: the runtime drops a command
// whose generation moved on, and the room refuses one with a stale revision.
bool Again(int seconds) { return s_run.waited % (seconds * kSecond) == 1; }
// Each side its own name, since a room refuses a second member of the same
// name; a fixed delay, so Ready does not wait for the connection check; one round.
netplay::PlayerPreferences RoomPreferences() {
	netplay::PlayerPreferences preferences = Now()->preferences;
	preferences.displayName = Host() ? "SelfTest host" : "SelfTest guest";
	preferences.autoInputDelay = false;
	preferences.inputDelay = 2;
	preferences.tableRules.roundCount = 1;
	return preferences;
}
std::string ReadInvitation() {
	std::ifstream in(s_run.inviteFile);
	std::string invitation;
	std::getline(in, invitation);
	return invitation;
}

void Begin() {
	s_run.resultFile = Environment(L"SF4E_SELFTEST_RESULT");
	// A run that cannot leave its outcome is not run: the game closes at once.
	std::ofstream probe(s_run.resultFile, std::ios::trunc);
	if (s_run.resultFile.empty() || !probe) {
		Fail("result file", "'" + platform::WideToUtf8(s_run.resultFile) + "' cannot be written", Stage::Closing);
		return;
	}
	if (Requested() == Test::Hitboxes) {
		s_run.dump.open(s_run.resultFile + L".boxes", std::ios::binary | std::ios::trunc);
		s_run.only = platform::WideToUtf8(Environment(L"SF4E_SELFTEST_ONLY"));
		const std::wstring fighters = Environment(L"SF4E_SELFTEST_FIGHTERS");
		int one = 0, two = 0;
		if (swscanf_s(fighters.c_str(), L"%d,%d", &one, &two) == 2 && one >= 0 && one < 44 && two >= 0 && two < 44) { s_run.fighters[0] = one; s_run.fighters[1] = two; }
		else if (!fighters.empty()) spdlog::warn("SelfTest: '{}' is not two fighters; Ryu against Ryu", platform::WideToUtf8(fighters));
	}
	spdlog::info("SelfTest: {}, result to '{}'", platform::WideToUtf8(Environment(L"SF4E_SELFTEST")), platform::WideToUtf8(s_run.resultFile));
	Enter(Stage::ToMainMenu);
}


// The room test, one step a tick from the main menu on. The host opens the
// room and the guest joins it; the rest is the same on both sides.
void TickRoom() {
	Run& run = s_run;
	const auto now = Now();
	const int seat = Host() ? 0 : 1;
	switch (run.stage) {
	case Stage::RoomSetup: {
		// A room needs a controller: the keyboard, when none was kept, asked
		// for as the controller page asks.
		if (!now->controllerReady) {
			if (Again(3)) { RuntimeCommand request; request.command.generation = now->session.generation; request.inputAction = input::Action::UseKeyboard; NetplayFacade::SubmitRuntimeCommand(std::move(request)); }
			if (Late(15)) Fail("controller", "none could be assigned", Stage::Closing);
			break;
		}
		if (!now->helperReady || !now->canOpenRoom) { if (Late(60)) Fail("room", "rooms never became available: " + now->helperError, Stage::Closing); break; }
		run.inviteFile = Environment(L"SF4E_SELFTEST_INVITE");
		if (run.inviteFile.empty()) { Fail("room", "SF4E_SELFTEST_INVITE is not set", Stage::Closing); break; }
		RuntimeCommand request = Command(Host() ? CommandKind::HostRoom : CommandKind::SavePreferences);
		request.preferences = RoomPreferences();
		request.displayName = request.preferences.displayName;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		Enter(Host() ? Stage::RoomOpening : Stage::RoomJoin);
		break;
	}
	case Stage::RoomJoin: {
		// The guest's settings first, then the invitation from the other PC.
		if (now->preferences.displayName != RoomPreferences().displayName) { if (Late(10)) Fail("room", "the guest's settings were not taken", Stage::Closing); break; }
		const std::string invitation = ReadInvitation();
		if (invitation.empty()) { if (Late(180)) Fail("room", "no invitation arrived from the host", Stage::Closing); break; }
		RuntimeCommand request = Command(CommandKind::JoinInvite);
		request.command.invitation = invitation;
		NetplayFacade::SubmitRuntimeCommand(std::move(request));
		Enter(Stage::RoomOpening);
		break;
	}
	case Stage::RoomOpening:
		if (now->session.room == netplay::RoomState::Joined && (!Host() || !now->invitation.empty())) {
			if (Host()) {
				std::ofstream out(run.inviteFile, std::ios::trunc);
				out << now->invitation << '\n';
				out.flush();
				if (!out) { Fail("room", "the invitation could not be written", Stage::Closing); break; }
			}
			Say(true, "room", now->room.name);
			Enter(Stage::RoomSeat);
		}
		else if (!now->session.error.empty()) Fail("room", now->session.error, Stage::Closing);
		else if (Late(60)) Fail("room", "never joined", Stage::Closing);
		break;
	case Stage::RoomSeat:
		// The host takes Player 1's seat at the first table, the guest Player 2's.
		if (now->localSlot == seat) { Say(true, "seat", "Player " + std::to_string(seat + 1)); Enter(Stage::RoomReady); break; }
		if (Again(3)) { RuntimeCommand request = Room(room::ActionKind::Queue); request.roomAction.seat = static_cast<std::int8_t>(seat); NetplayFacade::SubmitRuntimeCommand(std::move(request)); }
		if (Late(30)) Fail("seat", now->helperError.empty() ? "never taken" : now->helperError, Stage::RoomLeaving);
		break;
	case Stage::RoomReady: {
		// Ready as Ryu on the first stage once both seats are filled, until the
		// match is live: a Ready parked while the room settles is sent again.
		if (now->session.match == netplay::MatchState::Playing) { Say(true, "match live"); Enter(Stage::RoomPlaying); break; }
		const auto& table = now->room.tables[0];
		if (table.p1 && table.p2 && now->session.match == netplay::MatchState::None && !now->session.readyPending && Again(5)) {
			RuntimeCommand request = Command(CommandKind::Ready);
			request.character.charaID = 0; request.character.unc_edition = Dimps::Game::Battle::ED_USF4;
			NetplayFacade::SubmitRuntimeCommand(std::move(request));
		}
		if (Late(120)) Fail("match", now->readyFailure.empty() ? "never went live" : now->readyFailure, Stage::RoomLeaving);
		break;
	}
	case Stage::RoomPlaying:
		// Five seconds of the game, then the host ends it, as End game does.
		if (Late(5)) Enter(Stage::RoomEnding);
		break;
	case Stage::RoomEnding:
		if (now->session.match != netplay::MatchState::Playing && now->session.match != netplay::MatchState::Preparing) { Say(true, "match end"); Enter(Stage::RoomLeaving); }
		else if (Host() && Again(5)) NetplayFacade::SubmitRuntimeCommand(Room(room::ActionKind::AbortMatch));
		else if (Late(60)) Fail("match end", "the game never ended", Stage::RoomLeaving);
		break;
	case Stage::RoomLeaving:
		if (now->session.room == netplay::RoomState::Idle) { Say(true, "left the room"); Finish(); break; }
		if (Again(3)) NetplayFacade::SubmitRuntimeCommand(Command(CommandKind::LeaveRoom));
		if (Late(30)) Fail("leave", "the room never let go", Stage::Closing);
		break;
	default:
		break;
	}
}

}

Test Requested() {
	static const Test test = [] {
		const std::wstring name = Environment(L"SF4E_SELFTEST");
		const Test parsed = Parse(name);
		if (!name.empty() && parsed == Test::None) spdlog::warn("SelfTest: no test named '{}'", platform::WideToUtf8(name));
		return parsed;
	}();
	return test;
}

void Tick(bool atMainMenu, const std::string& screen) {
	Run& run = s_run;
	if (run.stage == Stage::Off) { if (Requested() == Test::None) return; Begin(); }
	run.waited++;
	if (run.stage >= Stage::RoomSetup && run.stage < Stage::Closing) { TickRoom(); return; }
	const training::View view = training::ReadView();
	const bool neutral = view.meter.current[0].valid && training::ClassifyStatus(view.meter.current[0].status) == training::Phase::Neutral;
	if (run.stage >= Stage::BoxSave && run.stage <= Stage::BoxRun) { TickBoxes(view, neutral); return; }
	switch (run.stage) {
	case Stage::Off:
		break;
	case Stage::ToMainMenu:
		// The title screen learns from its Start which device, and so which
		// profile, plays, so a real key is sent from outside
		// (scripts/run-selftest.ps1); nothing here answers the title screen.
		WatchScreen(screen);
		if (atMainMenu) {
			Say(true, "main menu");
			if (Requested() == Test::RoomHost || Requested() == Test::RoomGuest) Enter(Stage::RoomSetup);
			else if (EnterBattle()) Enter(Stage::ToBattle); else Fail("battle", "the main menu was not in front", Stage::Closing);
		}
		else if (Late(180)) Fail("main menu", "never reached", Stage::Closing);
		break;
	case Stage::ToBattle:
		// The lab is up and the fighter has stood for a second, so the round is being fought.
		if (view.available && view.ready && neutral) run.held++; else run.held = 0;
		if (run.held > kSecond) {
			Say(true, "battle", std::string(Dimps::characterNames[run.fighters[0]]) + " against " + Dimps::characterNames[run.fighters[1]] + ", the lab is in");
			// Playback repeats by default; every step here plays once.
			if (Submit(Action::Loop)) Enter(Requested() == Test::Hitboxes ? Stage::BoxSave : Stage::Move); else Fail("battle", "the lab did not take Loop off", Stage::Closing);
		}
		else if (Late(120)) Fail("battle", view.available ? "the round never began" : "the lab never came up in the battle", Stage::Closing);
		break;
	case Stage::Move:
		Play("2LP", "Ryu crouching LP", Stage::Measure, Stage::Record);
		break;
	case Stage::Starting:
		// The playback has to begin before its end can be waited for.
		if (view.mode == Mode::Playback) Enter(Stage::Settling);
		else if (Late(5)) Fail(run.playing, "the playback never started", run.otherwise);
		break;
	case Stage::Settling:
		if (view.mode == Mode::Idle && neutral) run.held++; else run.held = 0;
		if (run.held >= kSecond / 2) Enter(run.then);
		else if (Late(20)) Fail(run.playing, "the move never ended", run.otherwise);
		break;
	case Stage::Measure:
		Measure(view);
		Enter(Stage::Record);
		break;
	case Stage::Record:
		// The lab's record control: standby first, then recording, then stop.
		// Eight seconds are recorded and played back for five, long enough for
		// the HUD's strip to be seen.
		if (Submit(Action::AdvanceRecording)) Enter(Stage::Standby); else Fail("record", "the lab did not take the command", Stage::SavePosition);
		break;
	case Stage::Standby:
		if (view.mode != Mode::Standby) { if (Late(5)) Fail("record", "standby never began", Stage::SavePosition); break; }
		if (Submit(Action::AdvanceRecording)) { run.recorded = 0; Enter(Stage::Recording); } else Fail("record", "the lab did not take the command", Stage::SavePosition);
		break;
	case Stage::Recording:
		if (view.mode == Mode::Recording && run.recorded < kRecorded && ++run.recorded == kRecorded) Submit(Action::AdvanceRecording);
		else if (run.recorded == kRecorded && view.mode == Mode::Idle) {
			const int length = view.lengths[view.selected];
			Say(length >= kRecorded - kSecond && length <= kRecorded + kSecond, "record", std::to_string(length) + " frames in slot " + std::to_string(view.selected + 1));
			if (Submit(Action::Play)) { run.reached = 0; Enter(Stage::Replaying); } else Fail("play recording", "the lab did not take the command", Stage::SavePosition);
		}
		else if (Late(20)) { Submit(Action::Stop); Fail("record", "the recording never started or never stopped", Stage::SavePosition); }
		break;
	case Stage::Replaying:
		if (view.mode == Mode::Playback && view.cursor > run.reached) run.reached = view.cursor;
		if (!Late(5)) break;
		Say(run.reached > 2 * kSecond, "play recording", "reached frame " + std::to_string(run.reached) + " of " + std::to_string(view.lengths[view.selected]));
		Submit(Action::Stop);
		Enter(Stage::SavePosition);
		break;
	case Stage::SavePosition:
		// The playback just stopped has to be over before the position is saved.
		if (view.mode != Mode::Idle) { if (Late(5)) Fail("save position", "the playback never stopped", Stage::Closing); break; }
		run.saved = view.x[0];
		if (Submit(Action::Save)) Enter(Stage::Dash); else Fail("save position", "the lab did not take the command", Stage::Closing);
		break;
	case Stage::Dash:
		// A forward dash moves Player 1 away from where the position was saved.
		if (view.checkpoint) Play("66", "reset position", Stage::Dashed, Stage::Closing);
		else if (Late(5)) Fail("save position", "nothing was saved", Stage::Closing);
		break;
	case Stage::Dashed:
		run.dashed = view.x[0];
		if (run.dashed - run.saved < 0.1f) Fail("reset position", "the dash did not move the fighter, so the reset proves nothing", Stage::Closing);
		else if (Submit(Action::Restore)) Enter(Stage::Reset);
		else Fail("reset position", "the lab did not take the command", Stage::Closing);
		break;
	case Stage::Reset:
		if (!Late(1)) break;
		Say(std::fabs(view.x[0] - run.saved) < 0.01f, "reset position", "Player 1 at " + std::to_string(view.x[0]) +
			", saved at " + std::to_string(run.saved) + ", dashed to " + std::to_string(run.dashed));
		Finish();
		break;
	case Stage::Closing:
		// A game that does not close on being asked is ended. It ends with 0,
		// as a closed game does, so the launcher shows no recovery screen: the
		// outcome is in the result file.
		if (Late(20)) { spdlog::warn("SelfTest: the game did not close; ending it"); TerminateProcess(GetCurrentProcess(), 0); }
		break;
	}
}

} }
