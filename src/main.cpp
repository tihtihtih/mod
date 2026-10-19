#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {

enum class Mode { Off, Record, Replay };

struct MacroEvent {
	int frame;
	bool down;
	int button;
	bool p1;
	bool consumed = false;
	int releaseIdx = -1; // for presses: index of the matching release event
};

struct Pending {
	int frame;
	bool down;
	int button;
	bool p1;
};

struct State {
	Mode active = Mode::Off;
	std::vector<MacroEvent> events;
	std::vector<Pending> pending;
	bool injecting = false;
};

State& st() {
	static State s;
	return s;
}

Mode readMode() {
	auto m = Mod::get()->getSettingValue<std::string>("mode");
	if (m == "Record") return Mode::Record;
	if (m == "Replay") return Mode::Replay;
	return Mode::Off;
}

std::filesystem::path macroPath(int levelID) {
	return Mod::get()->getSaveDir() / (std::to_string(levelID) + ".macro");
}

bool isPlay(GJBaseGameLayer* layer) {
	auto pl = PlayLayer::get();
	return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
}

void saveMacro(int levelID) {
	std::ofstream out(macroPath(levelID));
	for (auto const& e : st().events) {
		out << e.frame << ' ' << (e.down ? 1 : 0) << ' ' << e.button << ' ' << (e.p1 ? 1 : 0) << '\n';
	}
	log::info("Saved {} macro events for level {}", st().events.size(), levelID);
}

bool loadMacro(int levelID) {
	std::ifstream in(macroPath(levelID));
	if (!in) return false;

	auto& events = st().events;
	events.clear();

	int frame, down, button, p1;
	while (in >> frame >> down >> button >> p1) {
		MacroEvent e{};
		e.frame = frame;
		e.down = down != 0;
		e.button = button;
		e.p1 = p1 != 0;
		events.push_back(e);
	}

	// pair each press with the next unpaired release of the same button/player
	std::vector<bool> used(events.size(), false);
	for (size_t i = 0; i < events.size(); i++) {
		if (!events[i].down) continue;
		for (size_t j = i + 1; j < events.size(); j++) {
			if (used[j] || events[j].down) continue;
			if (events[j].button == events[i].button && events[j].p1 == events[i].p1) {
				events[i].releaseIdx = static_cast<int>(j);
				used[j] = true;
				break;
			}
		}
	}
	return !events.empty();
}

} // namespace

class $modify(CWMPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();

		auto& s = st();
		s.pending.clear();
		s.active = Mode::Off;

		// practice mode checkpoints would desync the frame counter, so stay off there
		if (m_isPracticeMode) return;

		int levelID = m_level->m_levelID;
		auto mode = readMode();

		if (mode == Mode::Record) {
			s.events.clear();
			s.active = Mode::Record;
		}
		else if (mode == Mode::Replay) {
			if (loadMacro(levelID)) {
				s.active = Mode::Replay;
			}
			else {
				log::warn("No macro saved for level {}", levelID);
			}
		}
	}

	void levelComplete() {
		auto& s = st();
		if (s.active == Mode::Record && !s.events.empty()) {
			saveMacro(m_level->m_levelID);
		}
		PlayLayer::levelComplete();
	}
};

class $modify(CWMGameLayer, GJBaseGameLayer) {
	void processCommands(float dt) {
		auto& s = st();

		// fire any scheduled macro inputs that are due
		if (s.active == Mode::Replay && isPlay(this) && !s.pending.empty()) {
			int f = m_gameState.m_currentProgress;
			std::vector<Pending> due;
			std::vector<Pending> later;
			for (auto const& p : s.pending) {
				(p.frame <= f ? due : later).push_back(p);
			}
			s.pending = later;

			s.injecting = true;
			for (auto const& p : due) {
				GJBaseGameLayer::handleButton(p.down, p.button, p.p1);
			}
			s.injecting = false;
		}

		GJBaseGameLayer::processCommands(dt);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		auto& s = st();

		// our own injected inputs, or not in a level we're tracking
		if (s.injecting || s.active == Mode::Off || !isPlay(this)) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		int f = m_gameState.m_currentProgress;

		if (s.active == Mode::Record) {
			MacroEvent e{};
			e.frame = f;
			e.down = down;
			e.button = button;
			e.p1 = isPlayer1;
			s.events.push_back(e);
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		// Replay: real releases are ignored, the macro handles its own releases
		if (!down) return;

		int window = static_cast<int>(Mod::get()->getSettingValue<int64_t>("window"));

		int best = -1;
		for (size_t i = 0; i < s.events.size(); i++) {
			auto const& e = s.events[i];
			if (!e.down || e.consumed || e.button != button || e.p1 != isPlayer1) continue;
			if (std::abs(e.frame - f) <= window) {
				best = static_cast<int>(i);
				break; // events are in order, so this is the earliest match
			}
		}

		// click wasn't near any recorded click: swallow it
		if (best < 0) return;

		auto& e = s.events[best];
		e.consumed = true;

		// perform the macro's click at its recorded frame, or right now if we're already past it
		int fire = std::max(f, e.frame);
		if (fire <= f) {
			GJBaseGameLayer::handleButton(true, e.button, e.p1);
		}
		else {
			s.pending.push_back({fire, true, e.button, e.p1});
		}

		if (e.releaseIdx >= 0) {
			auto const& r = s.events[e.releaseIdx];
			s.pending.push_back({std::max(r.frame, fire), false, r.button, r.p1});
		}
	}
};
