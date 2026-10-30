#pragma once

#include <vector>

#include "data/EventLog.h"
#include "data/StateJson.h"
#include "game/GameState.h"

// Writes every touch to the event log. The hit is applied over the next ticks (Phase 0: speed still rises one tick
// after the touch), so each touch is logged 2 ticks later with the ball speed after the hit.
class TouchLogger
{
public:
	explicit TouchLogger(EventLog& log) : log_(log) {}

	void onTouch(const TouchEvent& e)
	{
		pending_.push_back(e);
	}

	void onTick(const Snapshot& s)
	{
		if (pending_.empty())
			return;
		std::vector<TouchEvent> keep;
		for (const auto& e : pending_)
		{
			if (s.tick < e.tick + kTicksAfter)
			{
				keep.push_back(e);
				continue;
			}
			const float after = s.ball ? s.ball->velocity.length() : -1.f;
			log_.write("touch", {
				{"tick", e.tick},
				{"carId", e.carId},
				{"car", e.carName},
				{"local", e.isLocal},
				{"bot", e.isBot},
				{"simulated", e.simulated},
				{"carLocation", statejson::toJson(e.carLocation)},
				{"ballLocation", statejson::toJson(e.ballLocation)},
				{"ballSpeedBefore", statejson::round2(e.ballVelocity.length())},
				{"ballSpeedAfter", statejson::round2(after)},
				{"ballKmhAfter", statejson::round2(after * 0.036f)},
			});
		}
		pending_ = std::move(keep);
	}

private:
	static constexpr uint64_t kTicksAfter = 2;

	EventLog& log_;
	std::vector<TouchEvent> pending_;
};
