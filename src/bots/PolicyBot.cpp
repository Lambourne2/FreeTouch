#include "pch.h"
#include "bots/PolicyBot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

#include "third_party/nlohmann/json.hpp"

bool PolicyBotConfig::load(const std::filesystem::path& file, PolicyBotConfig& out, std::string& error)
{
	std::ifstream in(file);
	if (!in)
	{
		error = "cannot open " + file.string();
		return false;
	}
	try
	{
		const auto j = nlohmann::json::parse(in);
		out.name = j.value("name", out.name);
		out.obs = j.value("obs", out.obs);
		out.heads = j.value("heads", out.heads);
		out.tickSkip = std::max(1, j.value("tickSkip", out.tickSkip));
		out.kickoff = j.value("kickoff", out.kickoff);
		out.stochasticKickoffs = j.value("stochasticKickoffs", out.stochasticKickoffs);
		if (j.contains("difficulty") && j["difficulty"].is_object())
			for (const auto& [level, k] : j["difficulty"].items())
			{
				if (!k.is_object())
					continue; // "_about"
				Knobs knobs;
				knobs.beta = k.value("beta", knobs.beta);
				knobs.reactionSeconds = k.value("reactionSeconds", knobs.reactionSeconds);
				knobs.maxSpeed = k.value("maxSpeed", knobs.maxSpeed);
				out.difficulty[level] = knobs;
			}
		if (j.contains("conventions"))
		{
			const auto& c = j["conventions"];
			out.rollSign = c.value("rollSign", 1.f);
			if (c.contains("angularVelocitySign") && c["angularVelocitySign"].size() == 3)
			{
				const auto& a = c["angularVelocitySign"];
				out.angularVelocitySign = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
			}
		}
	}
	catch (const std::exception& e)
	{
		error = e.what();
		return false;
	}
	return true;
}

void PolicyBot::reset(float beta, uint64_t seed)
{
	obs_.reset();
	action_ = {};
	ticksToNext_ = 0;
	kickoffIndex_ = -1;
	kickoffLogged_ = false;
	beta_ = std::clamp(beta, 0.f, 1.f);
	rng_ = Rng(seed);
	seen_.clear();
}

std::optional<CarInput> PolicyBot::tick(const Snapshot& s, uint64_t carId)
{
	if (!policy_ || !s.ball)
		return std::nullopt;
	const auto self = std::find_if(s.cars.begin(), s.cars.end(), [&](const CarState& c) { return c.id == carId; });
	if (self == s.cars.end())
		return std::nullopt;

	// A kickoff: the ball still at the centre, untouched.
	const BallState& ball = *s.ball;
	// (Only along the floor, as bot.py checks y == 0: the ball may settle up and down a hair after being placed.)
	const bool kickoffPause = std::fabs(ball.location.x) < 2.f && std::fabs(ball.location.y) < 2.f && ball.location.z < 130.f
		&& std::hypot(ball.velocity.x, ball.velocity.y) < 1.f;
	if (!kickoffPause)
		kickoffIndex_ = -1;
	else if (kickoffIndex_ >= 0)
		++kickoffIndex_;
	else if (kickoffIndex_ == -1 && config_.kickoff == "speedflip" && scriptedKickoff_)
	{
		// The taker is the bot's team's nearest car to the ball (ties: the one on the left goes, as in bot.py).
		const auto dist = [&](const CarState& c) { return std::hypot(c.location.x - ball.location.x, c.location.y - ball.location.y); };
		bool taker = true;
		for (const auto& c : s.cars)
		{
			if (c.id == carId || c.team != self->team)
				continue;
			const float d = dist(c), mine = dist(*self);
			if (d < mine - 10.f)
				taker = false;
			else if (std::fabs(d - mine) <= 10.f)
			{
				const bool otherIsLeft = self->team == 0 ? c.location.x < self->location.x : c.location.x > self->location.x;
				if (otherIsLeft)
					taker = false;
			}
		}
		kickoffIndex_ = taker ? 0 : -2;
	}
	const float betaNow = kickoffPause && config_.stochasticKickoffs ? 0.5f : beta_;
	++timingTicks_;

	// Reaction time: the ball and the other cars as they were reactionTicks_ ago (its own car as it is now).
	seen_.push_back({ball, s.cars});
	while (static_cast<int>(seen_.size()) > reactionTicks_ + 1)
		seen_.pop_front();
	const Seen& view = seen_.front();

	if (--ticksToNext_ <= 0)
	{
		ticksToNext_ = config_.tickSkip;
		// The bot first, then its teammates, then its opponents.
		necto::World w;
		w.ball = view.ball;
		w.ball.angularVelocity = {view.ball.angularVelocity.x * config_.angularVelocitySign.x,
			view.ball.angularVelocity.y * config_.angularVelocitySign.y, view.ball.angularVelocity.z * config_.angularVelocitySign.z};
		w.padAvailable = s.padAvailable;
		auto add = [&](const CarState& c) {
			necto::Car car;
			car.id = c.id;
			car.team = c.team;
			car.location = c.location;
			car.velocity = c.velocity;
			Rot r = c.rotation;
			r.roll = static_cast<int>(r.roll * config_.rollSign);
			necto::orientation(r, car.forward, car.up);
			car.angularVelocity = {c.angularVelocity.x * config_.angularVelocitySign.x,
				c.angularVelocity.y * config_.angularVelocitySign.y, c.angularVelocity.z * config_.angularVelocitySign.z};
			car.boost = std::clamp(c.boost / 100.f, 0.f, 1.f);
			car.onGround = c.onGround;
			car.hasFlip = c.hasFlip;
			w.cars.push_back(car);
		};
		add(*self);
		for (const auto& c : view.cars)
			if (c.id != carId && c.team == self->team)
				add(c);
		for (const auto& c : view.cars)
			if (c.team != self->team)
				add(c);

		const auto started = std::chrono::steady_clock::now();
		if (config_.obs == "nexto")
		{
			// Nexto: one pick from its action table (best, or sampled by the scaled odds when beta < 1).
			const necto::Obs o = nexto::build(w, action_);
			const std::vector<float> logits = policy_->forward(o.q, o.kv, o.entities);
			int pick = static_cast<int>(std::max_element(logits.begin(), logits.end()) - logits.begin());
			if (betaNow < 0.999f && !logits.empty())
			{
				const float scale = betaNow <= 0.001f ? 0.f : std::log((betaNow + 1.f) / (1.f - betaNow)) / std::log(3.f);
				const float mx = *std::max_element(logits.begin(), logits.end()) * scale;
				std::vector<double> p(logits.size());
				double sum = 0.0;
				for (size_t k = 0; k < logits.size(); ++k)
					sum += p[k] = std::exp(logits[k] * scale - mx);
				double r = rng_.next01() * sum;
				for (size_t k = 0; k < p.size(); ++k)
					if ((r -= p[k]) <= 0.0)
					{
						pick = static_cast<int>(k);
						break;
					}
			}
			action_ = nexto::tableAction(policy_->actionTable(), pick);
			lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			++timingDecisions_;
			timingMsSum_ += lastMs_;
			timingMsMax_ = std::max(timingMsMax_, lastMs_);
		}
		else
		{
		const necto::Obs o = obs_.build(w, action_);
		std::vector<float> logits = policy_->forward(o.q, o.kv, o.entities);
		if (betaNow < 0.999f)
		{
			// Looser play: sample each head from the logits scaled like Necto's bot (beta 0 would be random).
			const float scale = betaNow <= 0.001f ? 0.f : std::log((betaNow + 1.f) / (1.f - betaNow)) / std::log(3.f);
			static const int sizes[5] = {3, 3, 2, 2, 2};
			size_t at = 0;
			for (int h = 0; h < 5 && at + sizes[h] <= logits.size(); ++h)
			{
				float mx = -1e30f;
				for (int k = 0; k < sizes[h]; ++k)
					mx = std::max(mx, logits[at + k] * scale);
				std::vector<double> p(sizes[h]);
				double sum = 0.0;
				for (int k = 0; k < sizes[h]; ++k)
					sum += p[k] = std::exp(logits[at + k] * scale - mx);
				double r = rng_.next01() * sum;
				int pick = sizes[h] - 1;
				for (int k = 0; k < sizes[h]; ++k)
					if ((r -= p[k]) <= 0.0)
					{
						pick = k;
						break;
					}
				// One-hot logits so parseAction picks it.
				for (int k = 0; k < sizes[h]; ++k)
					logits[at + k] = k == pick ? 1.f : 0.f;
				at += sizes[h];
			}
		}
		action_ = necto::parseAction(logits);
		lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			++timingDecisions_;
			timingMsSum_ += lastMs_;
			timingMsMax_ = std::max(timingMsMax_, lastMs_);
		}
	}
	if (kickoffPause && kickoffIndex_ >= 0)
	{
		if (const auto scripted = speedflipAction(kickoffIndex_))
		{
			action_ = *scripted; // also the "previous action" its next observation sees, as in bot.py
			kickoffLogged_ = true;
		}
	}
	CarInput in;
	in.throttle = action_[0];
	in.steer = action_[1];
	in.pitch = action_[2];
	in.yaw = action_[3];
	in.roll = action_[4];
	in.jump = action_[5] > 0.f;
	in.boost = action_[6] > 0.f;
	in.handbrake = action_[7] > 0.f;
	// Top speed on the ground (easier levels): no boost above it. In the air it still boosts (aerials need it).
	if (maxSpeed_ > 0.f && self->onGround && self->velocity.length() > maxSpeed_)
		in.boost = false;
	return in;
}

std::optional<std::array<float, 8>> PolicyBot::speedflipAction(int tick)
{
	// Nexto's bot.py KICKOFF_CONTROLS (ticks at 120 Hz): {throttle, steer, pitch, yaw, roll, jump, boost, handbrake}.
	struct Part
	{
		int ticks;
		std::array<float, 8> action;
	};
	static const Part kParts[] = {
		{44, {1, 0, 0, 0, 0, 0, 1, 0}},       // boost straight at the ball
		{16, {1, -1, 0, 0, 0, 0, 1, 0}},      // steer left a touch
		{8, {1, 0, 0, 0, 0, 1, 1, 0}},        // jump
		{4, {1, 0, 0, 0, 0, 0, 1, 0}},        // release
		{4, {1, 0, -0.7f, 0.8f, 0, 1, 1, 0}}, // diagonal flip
		{52, {1, 0, 1, 0, 0, 0, 1, 0}},       // cancel it (pitch back)
		{40, {1, 0, 0.5f, 0, 1, 0, 0, 0}},    // roll to land
	};
	if (tick < 0)
		return std::nullopt;
	for (const auto& part : kParts)
	{
		if (tick < part.ticks)
			return part.action;
		tick -= part.ticks;
	}
	return std::nullopt;
}
