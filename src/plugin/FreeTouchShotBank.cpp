#include "pch.h"
#include "plugin/FreeTouch.h"

#include <algorithm>
#include <fstream>

#include "core/Rng.h"
#include "data/SafeFile.h"

// Shot Bank's turns: fill the rack's five slots by scoring (modes.shot_bank), then save those shots, each a scenario made
// from its recording. The scenario player runs every rep; this keeps the rack and says what comes next. The rack stays
// (on disk too) until the player replaces a shot or empties it, so nothing is lost to a reset or a stopped drill.

namespace
{
	constexpr double kCountdownFull = 5.0;   // seconds after the last slot fills
	constexpr double kCountdownPicked = 3.0; // seconds after going to the Defend tile
}

std::filesystem::path FreeTouch::shotRackFile()
{
	return game_->dataFolder() / "freetouch" / "shotbank" / "rack.json";
}

void FreeTouch::shotBankLoad()
{
	if (bankLoaded_)
		return;
	bankLoaded_ = true;
	const auto file = shotRackFile();
	std::error_code ec;
	if (std::filesystem::exists(file, ec))
	{
		std::ifstream in(file, std::ios::binary);
		const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
		if (!doc.is_discarded())
			shotbank::fromJson(doc, bankRack_);
		else
			safefile::setAside(file);
	}
	eventLog_.write("shot_rack_loaded", {{"shots", bankRack_.count()}});
	shotBankPublish();
}

void FreeTouch::shotBankSave()
{
	std::error_code ec;
	std::filesystem::create_directories(shotRackFile().parent_path(), ec);
	safefile::write(shotRackFile(), shotbank::toJson(bankRack_).dump() + "\n");
}

void FreeTouch::shotBankPublish()
{
	auto view = std::make_shared<ShotRackView>();
	view->full = bankRack_.full();
	for (int i = 0; i < shotbank::kShots; ++i)
	{
		ShotRackView::Slot slot;
		slot.filled = bankRack_.filled(i);
		slot.kmh = bankRack_.slot(i).kmh;
		slot.seconds = static_cast<float>(bankRack_.slot(i).track.seconds());
		const auto& frames = bankRack_.slot(i).track.frames;
		const size_t step = std::max<size_t>(1, frames.size() / 48);
		for (size_t f = 0; f < frames.size(); f += step)
			slot.path.push_back({frames[f].ball.location.x, frames[f].ball.location.y});
		view->slots.push_back(std::move(slot));
	}
	bankView_ = std::move(view);
}

void FreeTouch::shotBankObjective()
{
	std::string text;
	const int picked = bankRack_.selected() + 1;
	if (bankTurn_ == BankTurn::Defending)
		text = (bankChallenge_.empty() ? "Save your own shot: " : "Save " + bankFrom_ + "'s shot: ") + std::to_string(bankIndex_ + 1)
			+ " of " + std::to_string(bankIds_.size());
	else if (bankTurn_ != BankTurn::Shooting)
		text.clear();
	else if (bankRack_.onDefend())
	{
		const auto left = bankDefendTick_ > tick_ ? static_cast<int>(std::ceil((bankDefendTick_ - tick_) / 120.0)) : 0;
		text = "Defending in " + std::to_string(left) + "   |   D-pad left: redo a shot";
	}
	else if (bankRack_.filled(picked - 1))
		text = "Shot " + std::to_string(picked) + ": score to replace it   |   D-pad: pick another";
	else
		text = "Shot " + std::to_string(picked) + " of " + std::to_string(shotbank::kShots) + ": score it any way you like";
	player_->setObjectiveText(text);
}

void FreeTouch::shotBankRepStart(const ResolvedScenario& rep)
{
	if (rep.id != shotbank::kShootId && rep.id.rfind(shotbank::kDefendPrefix, 0) != 0)
	{
		shotBankStop();
		return;
	}
	shotBankLoad();
	if (bankTurn_ == BankTurn::Off)
	{
		bankTurn_ = BankTurn::Shooting;
		if (bankStartSlot_ >= 0)
			bankRack_.select(bankStartSlot_);
		else
			bankRack_.selectFirstEmpty();
		bankStartSlot_ = -1;
		eventLog_.write("shot_bank_start", {{"shots", bankRack_.count()}, {"slot", bankRack_.selected() + 1}});
	}
	if (bankTurn_ == BankTurn::Shooting)
	{
		bankRec_.clear();
		bankTouchTick_ = 0;
	}
	shotBankObjective();
	// The first time on each side, a line on what to do (the menu's tips cover the rest).
	const std::string tip = bankTurn_ == BankTurn::Shooting ? "shot_bank_shoot" : "shot_bank_defend";
	if (!tutorialsSeen_.count(tip))
	{
		tutorialsSeen_.insert(tip);
		saveUiSettings();
		hud_.showHint(bankTurn_ == BankTurn::Shooting ? "Score to bank a shot. D-pad left and right pick a slot"
			: "Get to the ball: touching it stops the shot", 7.f);
	}
}

void FreeTouch::shotBankStop()
{
	if (bankTurn_ == BankTurn::Off)
		return;
	eventLog_.write("shot_bank_stopped", {{"shots", bankRack_.count()}, {"defending", bankTurn_ == BankTurn::Defending}});
	bankTurn_ = BankTurn::Off;
	bankDefendTick_ = 0;
	bankIds_.clear();
	bankSaved_.clear();
	bankChallenge_.clear();
	player_->setObjectiveText("");
}

void FreeTouch::shotBankResult(const ScenarioPlayer::Status& s)
{
	if (bankTurn_ == BankTurn::Defending)
	{
		// A friend's bank: the first go at each shot is the one that counts, whatever a retry does.
		if (bankIndex_ < bankSaved_.size() && (bankChallenge_.empty() || bankSaved_[bankIndex_] == 0))
			bankSaved_[bankIndex_] = s.outcome > 0 ? 1 : -1;
		const auto saved = std::count(bankSaved_.begin(), bankSaved_.end(), 1);
		eventLog_.write("shot_bank_save", {{"shot", bankIndex_ + 1}, {"result", s.resultLabel}, {"saved", saved}});
		if (bankIndex_ + 1 >= bankIds_.size())
			player_->setResultDetail("You saved " + std::to_string(saved) + " of " + std::to_string(bankIds_.size())
				+ (bankChallenge_.empty() ? "" : " of " + bankFrom_ + "'s shots"));
		return;
	}
	if (bankTurn_ != BankTurn::Shooting || s.scenarioId != shotbank::kShootId || s.outcome <= 0)
		return;
	if (bankRack_.onDefend())
	{
		player_->setResultDetail("All five are banked: this one doesn't count");
		return;
	}
	shotbank::Shot shot = bankTouchTick_ != 0 ? shotbank::make(bankRec_.track(), s.resultKmh) : shotbank::Shot{};
	if (shot.track.frames.empty() || s.resultKmh < shotbank::kMinKmh)
	{
		eventLog_.write("shot_bank_refused", {{"kmh", s.resultKmh}, {"frames", shot.track.frames.size()}, {"touched", bankTouchTick_ != 0}});
		player_->setResultDetail("Too soft to bank: hit it harder");
		return;
	}
	const int slot = bankRack_.selected() + 1;
	eventLog_.write("shot_banked", {{"slot", slot}, {"kmh", s.resultKmh}, {"seconds", shot.track.seconds()},
		{"frames", shot.track.frames.size()}, {"replaced", bankRack_.filled(slot - 1)}});
	const bool replaced = bankRack_.store(std::move(shot));
	player_->setResultDetail(replaced ? "Replaced shot " + std::to_string(slot)
		: "Banked " + std::to_string(bankRack_.count()) + " of " + std::to_string(shotbank::kShots));
	if (bankRack_.onDefend())
		bankDefendTick_ = tick_ + static_cast<uint64_t>(kCountdownFull * 120.0);
	shotBankSave();
	shotBankPublish();
	publishStatus();
}

void FreeTouch::shotBankTick()
{
	if (bankTurn_ != BankTurn::Shooting)
		return;
	if (bankDefendTick_ == 0)
		return;
	if (!bankRack_.onDefend())
	{
		bankDefendTick_ = 0;
		return;
	}
	if (tick_ >= bankDefendTick_)
	{
		bankDefendTick_ = 0;
		shotBankDefend();
	}
	else if (tick_ % 30 == 0)
	{
		shotBankObjective();
	}
}

void FreeTouch::shotBankStep(int delta)
{
	if (bankTurn_ == BankTurn::Defending)
	{
		if (delta > 0)
			shotBankAdvance();
		else if (bankIndex_ > 0 && bankChallenge_.empty())
			player_->play(bankIds_[--bankIndex_], "medium", std::nullopt);
		return;
	}
	bankRack_.step(delta);
	bankDefendTick_ = bankRack_.onDefend() ? tick_ + static_cast<uint64_t>(kCountdownPicked * 120.0) : 0;
	eventLog_.write("shot_rack_pick", {{"slot", bankRack_.selected() + 1}, {"defend", bankRack_.onDefend()}});
	player_->retry(); // a fresh start for this slot
}

bool FreeTouch::shotBankAdvance()
{
	if (bankTurn_ != BankTurn::Defending)
		return false;
	if (++bankIndex_ < bankIds_.size())
		return player_->play(bankIds_[bankIndex_], "medium", std::nullopt);
	const auto saved = std::count(bankSaved_.begin(), bankSaved_.end(), 1);
	eventLog_.write("shot_bank_done", {{"saved", saved}, {"of", bankIds_.size()}, {"from", bankFrom_}});
	if (!bankChallenge_.empty())
	{
		shotBankFinishChallenge();
		return true;
	}
	shotBankStop(); // back to the rack, its shots still there
	return player_->play(shotbank::kShootId, "medium", std::nullopt);
}

void FreeTouch::shotBankFinishChallenge()
{
	std::string results(shotbank::kShots, 'X');
	int saved = 0;
	for (size_t i = 0; i < bankOrder_.size() && i < bankSaved_.size(); ++i)
		if (bankSaved_[i] > 0)
		{
			results[bankOrder_[i]] = 'S';
			++saved;
		}
	if (bankCounted_)
		banks_->finish(bankChallenge_, results, static_cast<int64_t>(std::time(nullptr)));
	const std::string line = "You saved " + std::to_string(saved) + " of " + std::to_string(bankOrder_.size()) + " of " + bankFrom_ + "'s shots"
		+ (bankCounted_ ? "" : " (practice)");
	shotBankStop();
	player_->stop();
	notice(line);
	if (!isWindowOpen_)
		cvarManager->executeCommand("togglemenu " + GetMenuName()); // the result, and how it compares
}

void FreeTouch::shotBankPlayChallenge(const std::string& challenge)
{
	banks_->fetch(challenge, static_cast<int64_t>(std::time(nullptr)),
		[this, challenge](const nlohmann::json& bank, const std::string& from, const std::string& results, const std::string& error) {
			if (!error.empty())
			{
				notice(error);
				return;
			}
			shotbank::Rack guest;
			if (!shotbank::fromShare(bank, guest))
			{
				eventLog_.write("bank_unreadable", {{"challenge", challenge}, {"from", from}});
				notice("That Shot Bank couldn't be read");
				return;
			}
			shotBankStop();
			bankGuest_ = std::move(guest);
			bankChallenge_ = challenge;
			bankFrom_ = from;
			bankCounted_ = results.empty();
			eventLog_.write("bank_challenge_start", {{"challenge", challenge}, {"from", from}, {"counted", bankCounted_}});
			startInFreeplay([this] { shotBankDefend(); publishStatus(); }, shotbank::kShootId);
		});
}

bool FreeTouch::shotBankDefend()
{
	shotBankLoad();
	const shotbank::Rack& rack = bankChallenge_.empty() ? bankRack_ : bankGuest_;
	if (!rack.full())
		return false;
	Rng rng(static_cast<uint64_t>(std::time(nullptr)) ^ tick_);
	std::vector<int> order(shotbank::kShots);
	for (int i = 0; i < shotbank::kShots; ++i)
		order[i] = i;
	for (size_t i = order.size(); i > 1; --i)
		std::swap(order[i - 1], order[static_cast<size_t>(rng.next01() * i) % i]);

	bankIds_.clear();
	bankOrder_.clear();
	const std::string carName = bankChallenge_.empty() ? "FT Past You" : "FT " + bankFrom_;
	for (size_t i = 0; i < order.size(); ++i)
	{
		const bool mirrored = rng.next01() < 0.5;
		const std::string id = shotbank::kDefendPrefix + std::to_string(++bankScenarios_);
		const nlohmann::json doc = shotbank::toScenario(rack.slot(order[i]), id, static_cast<int>(i) + 1, static_cast<int>(order.size()), mirrored, carName);
		std::string error;
		if (!content_.add(ContentLibrary::Kind::Scenario, doc, {}, error))
		{
			LOG("Free Touch: Shot Bank scenario refused: {}", error);
			eventLog_.write("shot_bank_scenario_refused", {{"id", id}, {"error", error}});
			continue;
		}
		bankIds_.push_back(id);
		bankOrder_.push_back(order[i]);
		eventLog_.write("shot_bank_order", {{"id", id}, {"slot", order[i] + 1}, {"mirrored", mirrored}});
	}
	if (bankIds_.empty())
	{
		shotBankStop();
		return false;
	}
	bankSaved_.assign(bankIds_.size(), 0);
	bankIndex_ = 0;
	bankDefendTick_ = 0;
	bankTurn_ = BankTurn::Defending;
	return player_->play(bankIds_.front(), "medium", std::nullopt);
}

void FreeTouch::shotBankAction(const std::string& action, int slot)
{
	shotBankLoad();
	if (action == "clear")
	{
		if (slot >= 0)
			bankRack_.clearSlot(slot);
		else
			bankRack_.clear();
		eventLog_.write("shot_rack_cleared", {{"slot", slot}});
		bankDefendTick_ = 0;
		shotBankSave();
		shotBankPublish();
		if (bankTurn_ == BankTurn::Shooting)
			shotBankObjective();
		publishStatus();
		return;
	}
	if (action == "defend")
	{
		shotBankStop();
		bankChallenge_.clear();
		startInFreeplay([this] { shotBankDefend(); publishStatus(); }, shotbank::kShootId);
		return;
	}
	bankStartSlot_ = slot;
	shotBankStop();
	bankChallenge_.clear();
	startInFreeplay([this] { startExercise(shotbank::kShootId, "medium", {}); }, shotbank::kShootId);
}

ShotRackHud FreeTouch::shotRackHud() const
{
	ShotRackHud r;
	if (bankTurn_ == BankTurn::Off)
		return r;
	if (bankTurn_ == BankTurn::Defending)
	{
		for (size_t i = 0; i < bankIds_.size(); ++i)
			r.tiles.push_back(bankSaved_[i] > 0 ? 2 : bankSaved_[i] < 0 ? 3 : 0);
		r.selected = static_cast<int>(bankIndex_);
		return r;
	}
	for (int i = 0; i < shotbank::kShots; ++i)
		r.tiles.push_back(bankRack_.filled(i) ? 1 : 0);
	r.selected = bankRack_.selected();
	r.defendTile = true;
	r.defendReady = bankRack_.full();
	return r;
}

void FreeTouch::drawShotPath(Canvas& canvas)
{
	// The picked slot's shot on the field, so you can see which one it is before replacing it.
	if (bankTurn_ != BankTurn::Shooting || bankRack_.onDefend() || !bankRack_.filled(bankRack_.selected()))
		return;
	const auto& frames = bankRack_.slot(bankRack_.selected()).track.frames;
	for (size_t i = 4; i < frames.size(); i += 4)
		canvas.line3d(frames[i - 4].ball.location, frames[i].ball.location, Rgba{255, 200, 80, 200}, 10.f, 3.f);
}
