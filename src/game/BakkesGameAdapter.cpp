#include "pch.h"
#include "game/BakkesGameAdapter.h"

#include "core/BoostPads.h"
#include "bakkesmod/core/bot_loadout_structs.h"
#include "bakkesmod/wrappers/GuiManagerWrapper.h"
#include "bakkesmod/wrappers/http/HttpWrapper.h"
#include "core/Utf.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h> // the clipboard (copyToClipboard)
#include <ShlObj.h>  // DROPFILES, the Downloads folder
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#include <commdlg.h> // the Open dialog (pickFile)
#pragma comment(lib, "Comdlg32.lib")
#include <cstring>

#include <algorithm>

// Behaviour below follows the Phase 0 results in docs/SDK_NOTES.md.

namespace
{
	constexpr auto kHookVehicleInput = "Function TAGame.Car_TA.SetVehicleInput"; // 120 Hz, once per car per tick
	constexpr auto kHookEventHitBall = "Function TAGame.Car_TA.EventHitBall";    // once per touch, caller = car
	// Boost pad pickups (caller = the pad). To verify in game which of these fire; both are handled the same way.
	constexpr const char* kPadPickupHooks[] = {
		"Function TAGame.VehiclePickup_TA.OnPickUp",
		"Function TAGame.VehiclePickup_TA.Pickup",
	};
	constexpr double kTicksPerSecond = 120.0;

	// Candidate names for goal / reset / kickoff events. Not verified yet: they are logged when they fire, so the
	// event log shows which ones exist and when the game resets things behind an exercise's back.
	constexpr const char* kGameEventHooks[] = {
		"Function TAGame.Ball_TA.Explode",
		"Function TAGame.Ball_TA.OnHitGoal",
		"Function TAGame.Ball_TA.EventHitGoal",
		"Function TAGame.GameEvent_Soccar_TA.EventGoalScored",
		"Function TAGame.GameEvent_Soccar_TA.OnBallHitGoal",
		"Function TAGame.GameEvent_Soccar_TA.ResetBalls",
		"Function TAGame.GameEvent_TA.ResetBalls",
		"Function TAGame.GameEvent_Soccar_TA.ResetPlayers",
		"Function TAGame.GameEvent_TA.ResetPlayers",
		"Function TAGame.GameEvent_Soccar_TA.StartNewRound",
		"Function GameEvent_Soccar_TA.Countdown.BeginState",
		"Function GameEvent_Soccar_TA.Active.StartRound",
		"Function TAGame.GameEvent_TA.EventCountdownBegin",
		"Function TAGame.Ball_TA.EventResetBall",
	};
	constexpr float kSettleSeconds = 0.05f; // a teleport lands on the next physics step; boost goes in after that
	// Last event of freeplay's kickoff reset (Countdown.BeginState -> StartNewRound -> ResetBalls -> ResetPlayers ->
	// Active.StartRound, all within 2 ticks; seen in the Phase 1 R3 test).
	constexpr auto kHookRoundStarted = "Function GameEvent_Soccar_TA.Active.StartRound";

	Vec3 toVec3(const Vector& v) { return {v.X, v.Y, v.Z}; }

	// Canvas on top of BakkesMod's CanvasWrapper (only valid during one draw callback).
	class BakkesCanvas : public Canvas
	{
	public:
		BakkesCanvas(CanvasWrapper& c, CameraWrapper camera) : c_(c)
		{
			const Vector2 size = c_.GetSize();
			w_ = static_cast<float>(size.X);
			h_ = static_cast<float>(size.Y);
			if (!camera.IsNull())
			{
				hasCamera_ = true;
				camLoc_ = camera.GetLocation();
				forward_ = RotatorToVector(camera.GetRotation());
			}
		}

		float width() const override { return w_; }
		float height() const override { return h_; }

		void fillRect(float x, float y, float w, float h, Rgba col) override
		{
			setColor(col);
			c_.SetPosition(Vector2F{x, y});
			c_.FillBox(Vector2F{w, h});
		}

		void line(float x1, float y1, float x2, float y2, float thickness, Rgba col) override
		{
			setColor(col);
			c_.DrawLine(Vector2F{x1, y1}, Vector2F{x2, y2}, thickness);
		}

		void text(float x, float y, const std::string& s, float scale, Rgba col, bool shadow) override
		{
			setColor(col);
			c_.SetPosition(Vector2F{x, y});
			c_.DrawString(s, scale, scale, shadow);
		}

		void textSize(const std::string& s, float scale, float& w, float& h) override
		{
			const Vector2F size = c_.GetStringSize(s, scale, scale);
			w = size.X;
			h = size.Y;
		}

		// From the Phase 0 drawing spike (SDK_NOTES): clip at a 50 uu near plane (closer points project tens of
		// thousands of px off screen and DrawLine fills wedges), then Liang-Barsky to the screen + 50 px.
		void line3d(const Vec3& from, const Vec3& to, Rgba col, float maxWidth, float minWidth) override
		{
			if (!hasCamera_)
				return;
			constexpr float kNearPlane = 50.f;
			Vector a = toVec(from), b = toVec(to);
			const float da = Vector::dot(a - camLoc_, forward_) - kNearPlane;
			const float db = Vector::dot(b - camLoc_, forward_) - kNearPlane;
			if (da < 0.f && db < 0.f)
				return;
			if (da < 0.f || db < 0.f)
			{
				const Vector hit = a + (b - a) * (da / (da - db));
				(da < 0.f ? a : b) = hit;
			}
			const float depthA = Vector::dot(a - camLoc_, forward_), depthB = Vector::dot(b - camLoc_, forward_);
			const Vector2F sa = c_.ProjectF(a), sb = c_.ProjectF(b);
			const float width = std::clamp(maxWidth * 1200.f / std::max(1.f, (depthA + depthB) * 0.5f),
				std::min(minWidth, maxWidth), maxWidth);
			setColor(col);

			// Cut out the parts hidden behind an occluder: inside its screen circle and farther away than its centre.
			const float dx = sb.X - sa.X, dy = sb.Y - sa.Y;
			const float len2 = dx * dx + dy * dy;
			std::vector<std::pair<float, float>> hidden;
			for (const auto& o : occluders_)
			{
				if (len2 < 1e-6f)
					break;
				const float fx = sa.X - o.x, fy = sa.Y - o.y;
				const float bq = 2.f * (fx * dx + fy * dy), cq = fx * fx + fy * fy - o.r * o.r;
				const float disc = bq * bq - 4.f * len2 * cq;
				if (disc <= 0.f)
					continue;
				const float sq = std::sqrt(disc);
				const float t0 = std::max(0.f, (-bq - sq) / (2.f * len2)), t1 = std::min(1.f, (-bq + sq) / (2.f * len2));
				if (t0 >= t1)
					continue;
				const float tm = (t0 + t1) * 0.5f;
				if (depthA + (depthB - depthA) * tm > o.depth)
					hidden.push_back({t0, t1});
			}
			std::sort(hidden.begin(), hidden.end());
			float t = 0.f;
			for (const auto& [h0, h1] : hidden)
			{
				if (h0 > t)
					drawClipped(sa, sb, t, h0, width);
				t = std::max(t, h1);
			}
			if (t < 1.f)
				drawClipped(sa, sb, t, 1.f, width);
		}

		void setOccluders(const std::vector<Occluder>& spheres) override
		{
			occluders_.clear();
			if (!hasCamera_)
				return;
			Vector right = Vector::cross(forward_, Vector(0.f, 0.f, 1.f));
			if (right.magnitude() < 1e-3f)
				right = Vector(1.f, 0.f, 0.f);
			right.normalize();
			for (const auto& s : spheres)
			{
				const Vector c = toVec(s.centre);
				const float depth = Vector::dot(c - camLoc_, forward_);
				if (depth < 60.f)
					continue;
				const Vector2F p = c_.ProjectF(c), e = c_.ProjectF(c + right * s.radius);
				const float r = std::sqrt((e.X - p.X) * (e.X - p.X) + (e.Y - p.Y) * (e.Y - p.Y));
				occluders_.push_back({p.X, p.Y, r, depth});
			}
		}

		bool project(const Vec3& world, float& x, float& y) override
		{
			if (!hasCamera_)
				return false;
			Vector p = toVec(world);
			const float depth = Vector::dot(p - camLoc_, forward_);
			const bool inFront = depth > 50.f;
			if (!inFront)
				p = p + forward_ * (100.f - 2.f * depth); // mirrored in front of the camera
			const Vector2F s = c_.ProjectF(p);
			x = s.X;
			y = s.Y;
			return inFront;
		}

		float depthOf(const Vec3& p) override
		{
			return hasCamera_ ? Vector::dot(toVec(p) - camLoc_, forward_) : 0.f;
		}

	private:
		void setColor(Rgba c)
		{
			c_.SetColor(static_cast<char>(c.r), static_cast<char>(c.g), static_cast<char>(c.b), static_cast<char>(c.a));
		}

		static Vector toVec(const Vec3& v) { return Vector(v.x, v.y, v.z); }

		// Draws the part [t0, t1] of the screen segment a-b, clipped to the screen + 50 px (Liang-Barsky).
		void drawClipped(const Vector2F& a, const Vector2F& b, float t0, float t1, float width)
		{
			constexpr float kMargin = 50.f;
			const float dx = b.X - a.X, dy = b.Y - a.Y;
			const float p[4] = {-dx, dx, -dy, dy};
			const float q[4] = {a.X + kMargin, w_ + kMargin - a.X, a.Y + kMargin, h_ + kMargin - a.Y};
			for (int i = 0; i < 4; ++i)
			{
				if (p[i] == 0.f)
				{
					if (q[i] < 0.f)
						return;
					continue;
				}
				const float r = q[i] / p[i];
				if (p[i] < 0.f) t0 = std::max(t0, r);
				else t1 = std::min(t1, r);
				if (t0 > t1)
					return;
			}
			c_.DrawLine(Vector2F{a.X + t0 * dx, a.Y + t0 * dy}, Vector2F{a.X + t1 * dx, a.Y + t1 * dy}, width);
		}

		struct ScreenOccluder { float x, y, r, depth; };
		std::vector<ScreenOccluder> occluders_;

		CanvasWrapper& c_;
		float w_ = 0.f, h_ = 0.f;
		bool hasCamera_ = false;
		Vector camLoc_;
		Vector forward_;
	};
	Vector toVector(const Vec3& v) { return Vector(v.x, v.y, v.z); }
	Rot toRot(const Rotator& r) { return {r.Pitch, r.Yaw, r.Roll}; }
	Rotator toRotator(const Rot& r) { return Rotator(r.pitch, r.yaw, r.roll); }

	// The car object is replaced on every freeplay reset; the PRI (the player) is not, so it is the stable id.
	uint64_t carId(CarWrapper& car)
	{
		PriWrapper pri = car.GetPRI();
		return pri ? static_cast<uint64_t>(pri.memory_address) : static_cast<uint64_t>(car.memory_address);
	}

	CarState readCar(CarWrapper& car, uintptr_t localAddress)
	{
		CarState c;
		c.id = carId(car);
		c.name = car.GetOwnerName();
		c.team = car.GetTeamNum2();
		c.isLocal = car.memory_address == localAddress;
		PriWrapper pri = car.GetPRI();
		c.isBot = pri && pri.GetbBot();
		c.body = car.GetLoadoutBody();

		c.location = toVec3(car.GetLocation());
		c.velocity = toVec3(car.GetVelocity());
		c.angularVelocity = toVec3(car.GetAngularVelocity());
		c.rotation = toRot(car.GetRotation());
		BoostWrapper boost = car.GetBoostComponent();
		c.boost = boost ? boost.GetCurrentBoostAmount() * 100.f : 0.f; // the game stores 0..1

		c.hasFlip = car.HasFlip() != 0;
		c.onGround = car.IsOnGround();
		c.onWall = car.IsOnWall();
		c.jumped = car.GetbJumped() != 0;
		c.doubleJumped = car.GetbDoubleJumped() != 0;
		c.wheelContacts = car.GetNumWheelContacts();
		c.worldWheelContacts = car.GetNumWheelWorldContacts();
		return c;
	}
}

BakkesGameAdapter::BakkesGameAdapter(std::shared_ptr<GameWrapper> gameWrapper) : gw_(std::move(gameWrapper)) {}

void BakkesGameAdapter::start()
{
	if (started_)
		return;
	started_ = true;

	// Hooks stay installed until the plugin unloads (BakkesMod removes them then). Never unhook from inside a
	// callback: that crashed the game in Phase 0.
	gw_->HookEventWithCaller<CarWrapper>(kHookVehicleInput, [this](CarWrapper car, void* params, std::string) {
		onVehicleInput(car, params);
	});
	gw_->HookEventWithCaller<CarWrapper>(kHookEventHitBall, [this](CarWrapper car, void*, std::string) {
		onHitBall(car);
	});
	gw_->HookEventPost(kHookRoundStarted, [this](std::string) {
		padTakenUntil_.fill(0); // a reset brings every pad back
		for (auto& listener : roundResetListeners_)
			listener();
	});
	for (const char* name : kPadPickupHooks)
	{
		gw_->HookEventWithCallerPost<ActorWrapper>(name, [this](ActorWrapper pad, void*, std::string eventName) {
			if (!pad)
				return;
			const Vector at = pad.GetLocation();
			const int i = boostpads::nearest(at.X, at.Y);
			if (i < 0 || tick_ < padTakenUntil_[i])
				return; // not a field pad, or already taken (both hooks fire for one pickup)
			padTakenUntil_[i] = tick_ + (boostpads::isBig(i) ? 1200 : 480);
			for (auto& listener : gameEventListeners_)
				listener(eventName, tick_);
		});
	}
	gw_->RegisterDrawable([this](CanvasWrapper canvas) {
		// Custom training: read every frame (no vehicle input, so no ticks, while a shot waits frozen).
		if (!trainingListeners_.empty())
			tickTraining();
		// Replays have no physics ticks (no vehicle input): their frames are read here, once per rendered frame.
		if (!replayFrameListeners_.empty() && gw_->IsInReplay())
		{
			const ReplayInfo info = replayInfo();
			const Snapshot s = snapshot();
			for (auto& listener : replayFrameListeners_)
				listener(s, info);
		}
		if (drawListeners_.empty())
			return;
		BakkesCanvas c(canvas, gw_->GetCamera());
		for (auto& listener : drawListeners_)
			listener(c);
	});
	for (const char* name : kGameEventHooks)
	{
		gw_->HookEvent(name, [this](std::string eventName) {
			for (auto& listener : gameEventListeners_)
				listener(eventName, tick_);
		});
	}
}

void BakkesGameAdapter::addTickListener(TickListener listener)
{
	tickListeners_.push_back(std::move(listener));
}

void BakkesGameAdapter::addTouchListener(TouchListener listener)
{
	touchListeners_.push_back(std::move(listener));
}

void BakkesGameAdapter::addGameEventListener(GameEventListener listener)
{
	gameEventListeners_.push_back(std::move(listener));
}

void BakkesGameAdapter::addRoundResetListener(std::function<void()> listener)
{
	roundResetListeners_.push_back(std::move(listener));
}

void BakkesGameAdapter::addDrawListener(std::function<void(Canvas&)> listener)
{
	drawListeners_.push_back(std::move(listener));
}

void BakkesGameAdapter::onVehicleInput(CarWrapper& caller, void* params)
{
	if (!caller)
		return;
	lastInputTick_[caller.memory_address] = tick_;

	// Bot input override: overwrite the input the game is about to apply (Phase 0: CarWrapper::SetInput loses to
	// the Psyonix AI; this drives the bot exactly). A bot gets about two calls per tick; every call is overridden.
	if (!botInputs_.empty() && params)
	{
		const auto it = botInputs_.find(carId(caller));
		if (it != botInputs_.end())
		{
			const CarInput& in = it->second;
			ControllerInput c{};
			c.Throttle = in.throttle;
			c.Steer = in.steer;
			c.Pitch = in.pitch;
			c.Yaw = in.yaw;
			c.Roll = in.roll;
			// The game takes a dodge's direction from these, not from pitch / yaw (left at 0, every flip was a double
			// jump). As RLBot maps them: stick forward (pitch -1) dodges forward, yaw dodges sideways.
			c.DodgeForward = -in.pitch;
			c.DodgeStrafe = in.yaw;
			c.Handbrake = in.handbrake ? 1 : 0;
			c.Jump = in.jump ? 1 : 0;
			c.ActivateBoost = in.boost ? 1 : 0;
			c.HoldingBoost = in.boost ? 1 : 0;
			c.Jumped = 0;
			*reinterpret_cast<ControllerInput*>(params) = c;
		}
	}

	// Fires once per car per tick (about twice per tick for a Psyonix bot): advance only on the local car's call.
	// Resolve the local car every time; it is a new object after each reset.
	CarWrapper local = gw_->GetLocalCar();
	if (!local || local.memory_address != caller.memory_address)
		return;

	if (localOverrideTicks_ > 0 && params)
	{
		// A tap for the player's car (custom training import), written like a bot's input.
		--localOverrideTicks_;
		ControllerInput c{};
		c.Throttle = localOverride_.throttle;
		c.Steer = localOverride_.steer;
		c.Pitch = localOverride_.pitch;
		c.Yaw = localOverride_.yaw;
		c.Roll = localOverride_.roll;
		c.Jump = localOverride_.jump ? 1 : 0;
		c.ActivateBoost = c.HoldingBoost = localOverride_.boost ? 1 : 0;
		c.Handbrake = localOverride_.handbrake ? 1 : 0;
		*reinterpret_cast<ControllerInput*>(params) = c;
	}
	CarInput in;
	if (params)
	{
		const ControllerInput& c = *reinterpret_cast<const ControllerInput*>(params);
		in.throttle = c.Throttle;
		in.steer = c.Steer;
		in.pitch = c.Pitch;
		in.yaw = c.Yaw;
		in.roll = c.Roll;
		in.jump = c.Jump != 0;
		in.boost = c.HoldingBoost != 0 || c.ActivateBoost != 0;
		in.handbrake = c.Handbrake != 0;
	}
	++tick_;
	if (tick_ % 1200 == 0) // drop cars not seen for 10 s (old car objects from resets)
	{
		std::erase_if(lastInputTick_, [this](const auto& kv) { return kv.second + 1200 < tick_; });
	}
	if (tickListeners_.empty())
		return;
	Snapshot snap = snapshot();
	if (params)
		snap.localInput = in;
	for (auto& listener : tickListeners_)
		listener(snap);
}

void BakkesGameAdapter::onHitBall(CarWrapper& caller)
{
	if (!caller || touchListeners_.empty())
		return;
	ServerWrapper server = currentServer();
	if (!server)
		return;
	BallWrapper ball = server.GetBall();
	if (!ball)
		return;

	CarWrapper local = gw_->GetLocalCar();
	const uintptr_t localAddress = local ? local.memory_address : 0;

	TouchEvent e;
	e.tick = tick_;
	e.carId = carId(caller);
	e.carName = caller.GetOwnerName();
	e.isLocal = caller.memory_address == localAddress;
	PriWrapper pri = caller.GetPRI();
	e.isBot = pri && pri.GetbBot();
	const auto seen = lastInputTick_.find(caller.memory_address);
	e.simulated = seen != lastInputTick_.end() && seen->second + 2 >= tick_;
	e.carLocation = toVec3(caller.GetLocation());
	e.ballLocation = toVec3(ball.GetLocation());
	e.ballVelocity = toVec3(ball.GetVelocity());

	for (auto& listener : touchListeners_)
		listener(e);
}

ServerWrapper BakkesGameAdapter::currentServer()
{
	if (gw_->IsInReplay())
		return gw_->GetGameEventAsReplay();
	if (gw_->IsInFreeplay())
		return gw_->GetCurrentGameState();
	return ServerWrapper(0);
}

void BakkesGameAdapter::tickTraining()
{
	if (!gw_->IsInCustomTraining())
	{
		training_ = {};
		pressFrames_ = 0;
		return;
	}
	applyPress();
	ServerWrapper server = gw_->GetGameEventAsServer();
	if (!server)
		return;
	TrainingEditorWrapper editor(server.memory_address);
	if (!editor)
		return;
	const int round = editor.GetActiveRoundNumber();
	if (!training_.active || round != training_.round)
	{
		// The pack's details: only when the shot changes (strings and a function call, not every tick).
		training_ = {};
		training_.active = true;
		training_.round = round;
		training_.rounds = editor.GetTotalRounds();
		GameEditorSaveDataWrapper save = editor.GetTrainingData();
		if (save)
		{
			TrainingEditorSaveDataWrapper data = save.GetTrainingData();
			if (data)
			{
				UnrealStringWrapper name = data.GetTM_Name();
				if (name)
					training_.name = name.ToString();
				UnrealStringWrapper code = data.GetCode();
				if (code)
					training_.code = code.ToString();
				UnrealStringWrapper creator = data.GetCreatorName();
				if (creator)
					training_.creator = creator.ToString();
				training_.type = data.GetType();
			}
		}
	}
	training_.secondsLeft = server.GetGameTimeRemaining();
	if (BallWrapper ball = server.GetBall())
	{
		training_.trajectoryVelocity = toVec3(ball.GetTrajectoryStartVelocity());
		training_.trajectoryLocation = toVec3(ball.GetTrajectoryStartLocation());
		training_.drawsTrajectory = ball.ShouldDrawTrajectory();
	}
	Snapshot s = snapshotOf(server);
	s.tick = ++trainingFrames_;
	for (auto& listener : trainingListeners_)
		listener(s, training_);
}

void BakkesGameAdapter::applyPress()
{
	// A press on the player's own controller (what custom training waits for to start a shot), every frame for a
	// few frames, then let go.
	if (pressFrames_ <= 0)
		return;
	PlayerControllerWrapper pc = gw_->GetPlayerController();
	if (!pc)
	{
		pressFrames_ = 0;
		return;
	}
	--pressFrames_;
	const bool on = pressFrames_ > 0;
	if (pressMethod_ == 23)
	{
		ControllerInput in = pc.GetVehicleInput();
		in.Throttle = on ? 1.f : 0.f;
		pc.SetVehicleInput(in);
	}
	else if (pressMethod_ == 24)
	{
		pc.SetbBoostPressed(on ? 1 : 0);
	}
	else if (pressMethod_ == 25)
	{
		pc.SetbJumpPressed(on ? 1 : 0);
	}
}

bool BakkesGameAdapter::trainingNextRound(int method)
{
	if (!gw_->IsInCustomTraining())
		return false;
	ServerWrapper server = gw_->GetGameEventAsServer();
	if (!server)
		return false;
	TrainingEditorWrapper editor(server.memory_address);
	if (!editor)
		return false;
	switch (method)
	{
	case 0:
		editor.IncrementRound(1);
		server.PlayerResetTraining();
		break;
	case 1:
		editor.IncrementRound(1);
		server.StartNewRound();
		break;
	case 2:
		editor.NextRound();
		break;
	case 10:
		server.PlayerResetTraining();
		break;
	case 11:
		server.StartNewRound();
		break;
	case 12:
		editor.ResetRound();
		break;
	case 20:
		server.StartRound();
		break;
	case 21:
		server.SetbRoundActive(1);
		break;
	case 22:
	{
		PlayerControllerWrapper pc = gw_->GetPlayerController();
		if (!pc)
			return false;
		server.SetPaused(pc, 0);
		break;
	}
	case 23:
	case 24:
	case 25:
		pressMethod_ = method;
		pressFrames_ = 8;
		break;
	default:
		return false;
	}
	return true;
}

void BakkesGameAdapter::addTrainingTickListener(std::function<void(const Snapshot&, const TrainingInfo&)> listener)
{
	trainingListeners_.push_back(std::move(listener));
}

Snapshot BakkesGameAdapter::snapshot()
{
	return snapshotOf(currentServer());
}

Snapshot BakkesGameAdapter::snapshotOf(ServerWrapper server)
{
	Snapshot s;
	s.tick = tick_;
	for (int i = 0; i < boostpads::kCount; ++i)
		s.padAvailable[i] = tick_ >= padTakenUntil_[i];
	s.time = tick_ / kTicksPerSecond;
	s.inFreeplay = gw_->IsInFreeplay();
	s.inReplay = gw_->IsInReplay();
	s.paused = gw_->IsPaused();

	if (!server)
		return s;

	if (BallWrapper ball = server.GetBall())
	{
		s.ball = BallState{
			toVec3(ball.GetLocation()),
			toVec3(ball.GetVelocity()),
			toVec3(ball.GetAngularVelocity()),
		};
	}

	CarWrapper local = gw_->GetLocalCar();
	const uintptr_t localAddress = local ? local.memory_address : 0;
	for (CarWrapper car : server.GetCars())
	{
		if (!car)
			continue;
		s.cars.push_back(readCar(car, localAddress));
		if (s.cars.back().isLocal)
			s.localCarIndex = static_cast<int>(s.cars.size()) - 1;
	}
	return s;
}

MmrReading BakkesGameAdapter::readMmr(int playlistId)
{
	MmrReading r;
	MMRWrapper mmr = gw_->GetMMRWrapper();
	if (!mmr.memory_address)
		return r;
	const UniqueIDWrapper id = gw_->GetUniqueID();
	r.synced = mmr.IsSynced(id, playlistId);
	if (!r.synced)
		return r;
	r.mmr = mmr.GetPlayerMMR(id, playlistId);
	const SkillRank rank = mmr.GetPlayerRank(id, playlistId);
	r.tier = rank.Tier;
	r.division = rank.Division;
	r.matches = rank.MatchesPlayed;
	return r;
}

void BakkesGameAdapter::addMmrListener(std::function<void()> listener)
{
	mmrListeners_.push_back(std::move(listener));
	if (mmrToken_)
		return;
	MMRWrapper mmr = gw_->GetMMRWrapper();
	if (!mmr.memory_address)
		return;
	std::unique_ptr<MMRNotifierToken> token = mmr.RegisterMMRNotifier([this](UniqueIDWrapper id) {
		if (id != gw_->GetUniqueID())
			return; // someone else's rating (a teammate, an opponent)
		for (auto& l : mmrListeners_)
			l();
	});
	mmrToken_ = std::shared_ptr<MMRNotifierToken>(std::move(token));
}

ReplayInfo BakkesGameAdapter::replayInfo()
{
	ReplayInfo info;
	if (!gw_->IsInReplay())
		return info;
	ReplayServerWrapper rs = gw_->GetGameEventAsReplay();
	if (!rs)
		return info;
	info.active = true;
	info.time = rs.GetReplayTimeElapsed();
	info.frame = rs.GetCurrentReplayFrame();
	ReplayWrapper rep = rs.GetReplay();
	if (rep)
	{
		info.name = rep.GetReplayName().ToString();
		info.map = rep.GetMapName();
		info.frames = rep.GetNumFrames();
	}
	ActorWrapper target = rs.GetViewTarget();
	if (target)
	{
		for (CarWrapper car : rs.GetCars())
		{
			if (!car)
				continue;
			PriWrapper pri = car.GetPRI();
			if (car.memory_address == target.memory_address || (pri && pri.memory_address == target.memory_address))
				info.focusCarId = carId(car);
		}
	}
	return info;
}

void BakkesGameAdapter::addReplayFrameListener(std::function<void(const Snapshot&, const ReplayInfo&)> listener)
{
	replayFrameListeners_.push_back(std::move(listener));
}

bool BakkesGameAdapter::inFreeplay()
{
	return gw_->IsInFreeplay();
}

bool BakkesGameAdapter::inReplay()
{
	return gw_->IsInReplay();
}

bool BakkesGameAdapter::inOnlineGame()
{
	return gw_->IsInOnlineGame();
}

void BakkesGameAdapter::loadFont(const std::string& name, const std::string& file, int size)
{
	auto gui = gw_->GetGUIManager();
	gui.LoadFont(name, file, size); // built by BakkesMod before a later frame; font() returns it then
}

void* BakkesGameAdapter::font(const std::string& name)
{
	return gw_->GetGUIManager().GetFont(name);
}

bool BakkesGameAdapter::copyToClipboard(const std::string& utf8)
{
	// CF_UNICODETEXT is UTF-16 with surrogate pairs, so emoji past U+FFFF survive (ImGui's clipboard drops them).
	const std::u16string text = utf::utf8ToUtf16(utf8);
	if (!OpenClipboard(nullptr))
		return false;
	EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(char16_t);
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	bool ok = false;
	if (mem)
	{
		if (void* p = GlobalLock(mem))
		{
			std::memcpy(p, text.c_str(), bytes);
			GlobalUnlock(mem);
			ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
		}
		if (!ok)
			GlobalFree(mem); // the clipboard owns it only once SetClipboardData succeeds
	}
	CloseClipboard();
	return ok;
}

bool BakkesGameAdapter::copyFileToClipboard(const std::filesystem::path& file)
{
	// CF_HDROP: a DROPFILES header, then the path (wide) ending in two nulls, as Explorer's Copy puts it there.
	const std::wstring path = std::filesystem::absolute(file).wstring();
	const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
	if (!OpenClipboard(nullptr))
		return false;
	EmptyClipboard();
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
	bool ok = false;
	if (mem)
	{
		if (auto* drop = static_cast<DROPFILES*>(GlobalLock(mem)))
		{
			drop->pFiles = sizeof(DROPFILES);
			drop->fWide = TRUE;
			std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
			GlobalUnlock(mem);
			ok = SetClipboardData(CF_HDROP, mem) != nullptr;
		}
		if (!ok)
			GlobalFree(mem);
	}
	CloseClipboard();
	return ok;
}

std::filesystem::path BakkesGameAdapter::downloadsFolder()
{
	PWSTR raw = nullptr;
	std::filesystem::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) && raw)
		out = raw;
	CoTaskMemFree(raw);
	return out;
}

void BakkesGameAdapter::pickFile(const std::filesystem::path& startDir, std::function<void(const std::filesystem::path&)> done)
{
	if (pickerBusy_.exchange(true) || pickerClosing_)
		return; // one is open already
	if (picker_.joinable())
		picker_.join(); // the last one has finished
	// Owned by the game's window (in front of it; the game is disabled while it is open, as with any Open dialog).
	HWND owner = GetForegroundWindow();
	picker_ = std::thread([this, owner, startDir, done = std::move(done)] {
		pickerThreadId_ = GetCurrentThreadId();
		const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		std::wstring file(4096, L'\0');
		const std::wstring dir = startDir.wstring();
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = owner;
		ofn.lpstrFilter = L"Free Touch shots (*.ftshot)\0*.ftshot\0All files (*.*)\0*.*\0";
		ofn.lpstrFile = file.data();
		ofn.nMaxFile = static_cast<DWORD>(file.size());
		ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
		ofn.lpstrTitle = L"Add a shot a friend shared";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_DONTADDTORECENT;
		const BOOL ok = GetOpenFileNameW(&ofn);
		if (SUCCEEDED(com))
			CoUninitialize();
		pickerThreadId_ = 0;
		if (!pickerClosing_)
			done(ok ? std::filesystem::path(file.c_str()) : std::filesystem::path());
		pickerBusy_ = false;
	});
}

void BakkesGameAdapter::closeFilePicker()
{
	// Unloading: close the dialog (it returns as cancelled) so its thread ends before the plugin's code goes away.
	pickerClosing_ = true;
	for (int i = 0; i < 100 && pickerBusy_; ++i)
	{
		if (const DWORD id = pickerThreadId_)
			EnumThreadWindows(id, [](HWND w, LPARAM) { PostMessageW(w, WM_CLOSE, 0, 0); return TRUE; }, 0);
		Sleep(20);
	}
	if (picker_.joinable())
	{
		if (pickerBusy_)
			picker_.detach(); // still open after 2 s: let it go rather than hang the game
		else
			picker_.join();
	}
}

void BakkesGameAdapter::http(const online::HttpRequest& request, online::HttpDone done)
{
	if (!*requestsWanted_)
		return;
	CurlRequest req;
	req.url = request.url;
	req.verb = request.method;
	req.headers = request.headers;
	req.body = request.body;
	++*requestsOut_;
	auto out = requestsOut_;
	auto wanted = requestsWanted_;
	// BakkesMod's curl, on its own thread (418 when the request itself failed).
	HttpWrapper::SendCurlRequest(req, [out, wanted, done = std::move(done)](int status, std::string body) {
		if (*wanted)
			done(status == 418 && body.empty() ? 0 : status, body);
		--*out;
	});
}

#if FT_SELF_UPDATE
void BakkesGameAdapter::download(const std::string& url, const std::filesystem::path& to, std::function<void(int status)> done)
{
	if (!*requestsWanted_)
		return;
	CurlRequest req;
	req.url = url;
	req.verb = "GET";
	req.headers["apikey"] = online::kPublishableKey;
	++*requestsOut_;
	auto out = requestsOut_;
	auto wanted = requestsWanted_;
	HttpWrapper::SendCurlRequest(req, to.wstring(), [out, wanted, done = std::move(done)](int status, std::wstring) {
		if (*wanted)
			done(status == 418 ? 0 : status);
		--*out;
	});
}
#endif

void BakkesGameAdapter::stopRequests()
{
	*requestsWanted_ = false;
	for (int i = 0; i < 150 && *requestsOut_ > 0; ++i)
		Sleep(20); // up to 3 s for replies still on their way (their callbacks are this plugin's code)
}

bool BakkesGameAdapter::setBall(const BallTarget& target)
{
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	BallWrapper ball = server.GetBall();
	if (!ball)
		return false;

	ball.SetLocation(toVector(target.location));
	ball.SetVelocity(toVector(target.velocity));
	ball.SetAngularVelocity(toVector(target.angularVelocity), false);
	return true;
}

bool BakkesGameAdapter::setLocalBoost(float percent)
{
	if (!gw_->IsInFreeplay())
		return false;
	CarWrapper car = gw_->GetLocalCar();
	if (!car)
		return false;
	BoostWrapper boost = car.GetBoostComponent();
	if (!boost)
		return false;
	boost.SetBoostAmount(std::clamp(percent, 0.f, 100.f) / 100.f * boost.GetMaxBoostAmount());
	return true;
}

bool BakkesGameAdapter::setLocalCar(const CarTarget& target)
{
	if (!gw_->IsInFreeplay())
		return false;
	CarWrapper car = gw_->GetLocalCar();
	if (!car)
		return false;

	// A full tank cannot take pads, so the sweep from the old spot to the new one leaves pads alone.
	float boostPercent = target.boost.value_or(0.f);
	if (BoostWrapper boost = car.GetBoostComponent())
	{
		const float maxBoost = boost.GetMaxBoostAmount();
		if (!target.boost && maxBoost > 0.f) // keep what the player had
			boostPercent = boost.GetCurrentBoostAmount() / maxBoost * 100.f;
		boost.SetBoostAmount(boost.GetMaxBoostAmount());
	}

	car.SetLocation(toVector(target.location));
	car.SetRotation(toRotator(target.rotation));
	car.SetVelocity(toVector(target.velocity));
	car.SetAngularVelocity(toVector(target.angularVelocity), false);

	gw_->SetTimeout([boostPercent](GameWrapper* g) {
		if (!g->IsInFreeplay())
			return;
		CarWrapper c = g->GetLocalCar();
		if (!c)
			return;
		if (BoostWrapper boost = c.GetBoostComponent())
			boost.SetBoostAmount(std::clamp(boostPercent, 0.f, 100.f) / 100.f * boost.GetMaxBoostAmount());
	}, kSettleSeconds);
	return true;
}

bool BakkesGameAdapter::loadFreeplay(const std::string& map)
{
	// Never leaves an online match; only map names made of letters, digits and '_' (it goes into a travel URL).
	if (gw_->IsInOnlineGame() || gw_->IsInFreeplay() || map.empty())
		return false;
	if (!std::all_of(map.begin(), map.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
		return false;
	gw_->ExecuteUnrealCommand("start " + map + "?Game=TAGame.GameInfo_Tutorial_TA?GameTags=Freeplay");
	return true;
}

bool BakkesGameAdapter::poseLocalCar(const CarTarget& target)
{
	if (!gw_->IsInFreeplay())
		return false;
	CarWrapper car = gw_->GetLocalCar();
	if (!car)
		return false;
	car.SetLocation(toVector(target.location));
	car.SetRotation(toRotator(target.rotation));
	car.SetVelocity(toVector(target.velocity));
	car.SetAngularVelocity(toVector(target.angularVelocity), false);
	return true;
}

bool BakkesGameAdapter::setBallHidden(bool hidden)
{
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	BallWrapper ball = server.GetBall();
	if (!ball)
		return false;
	// Phase 0 hide test: SetHidden2 and SetbHiddenSelf do nothing; SetDrawScale hides the mesh but not its shadow and
	// floor ring; SetBallScale(0.01) leaves a ball too small to see or touch.
	// Scaling back to 1 does not fully restore the ball: wheels slid off it (no flip resets) until freeplay was left.
	// So un-hiding spawns a fresh ball instead; the caller places it again (the next tick as well).
	if (hidden)
		ball.SetBallScale(0.01f);
	else
	{
		ball.SetBallScale(1.f);
		server.ResetBalls();
	}
	return true;
}

std::vector<std::string> BakkesGameAdapter::keysDown(const std::vector<std::string>& keyNames)
{
	std::vector<std::string> down;
	for (const auto& name : keyNames)
	{
		auto it = keyIndex_.find(name);
		if (it == keyIndex_.end())
			it = keyIndex_.emplace(name, gw_->GetFNameIndexByString(name)).first;
		if (gw_->IsKeyPressed(it->second))
			down.push_back(name);
	}
	return down;
}

bool BakkesGameAdapter::explodeBall(int side, bool creditPlayer)
{
	// Phase 1 spike: BallWrapper::Explode plays the scorer's goal explosion and sound with no score, slow motion or
	// replay; afterwards the ball object ignores SetLocation until ResetBalls() spawns a new one.
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	BallWrapper ball = server.GetBall();
	if (!ball)
		return false;
	GoalWrapper net(0);
	for (GoalWrapper goal : server.GetGoals())
	{
		if (goal && (goal.GetLocation().Y > 0.f) == (side > 0))
			net = goal;
	}
	CarWrapper car = gw_->GetLocalCar();
	PriWrapper scorer = (creditPlayer && car) ? car.GetPRI() : PriWrapper(0);
	Vector at = ball.GetLocation();
	ball.Explode(net, at, scorer);
	return true;
}

bool BakkesGameAdapter::localCarReady()
{
	CarWrapper car = gw_->GetLocalCar();
	if (!car)
		return false;
	PriWrapper pri = car.GetPRI();
	return !pri || pri.GetRespawnTimeRemaining() <= 0;
}

bool BakkesGameAdapter::setBotBody(uint64_t id, int body)
{
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	for (CarWrapper car : server.GetCars())
	{
		if (!car || carId(car) != id)
			continue;
		PriWrapper pri = car.GetPRI();
		if (!pri || !pri.GetbBot())
			return false;
		// SpawnBot's body argument is not always honoured (the game picks a preset); the loadout sets it.
		BotLoadoutData loadout;
		loadout.products[0] = body; // slot 0 = body
		loadout.team = static_cast<unsigned char>(car.GetTeamNum2());
#pragma warning(push)
#pragma warning(disable : 4996) // BakkesMod marks SetBotLoadout experimental
		gw_->SetBotLoadout(pri, loadout);
#pragma warning(pop)
		return true;
	}
	return false;
}

bool BakkesGameAdapter::setBotTeam(uint64_t id, int team)
{
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	for (CarWrapper car : server.GetCars())
	{
		if (!car || carId(car) != id)
			continue;
		PriWrapper pri = car.GetPRI();
		if (!pri || !pri.GetbBot())
			return false;
		if (server.GetTeams().Count() < 2)
			return false; // this freeplay has no second team to move to
		pri.ServerChangeTeam(team);
		return true;
	}
	return false;
}

bool BakkesGameAdapter::spawnBot(const std::string& name)
{
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	server.SpawnBot(23, name); // Octane; CanSpawnBots() returns false in freeplay but this works (Phase 0)
	return true;
}

float BakkesGameAdapter::setBotSkill(float skill)
{
	if (!gw_->IsInFreeplay())
		return -1.f;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return -1.f;
	server.SetBotSkill(skill);   // the game-wide value new bots read
	server.SetBotSkill2(skill);  // presumably also applies it to bots already in the game (to verify)
	return server.GetBotSkill();
}

int BakkesGameAdapter::removeBots(const std::string& namePrefix)
{
	if (!gw_->IsInFreeplay())
		return 0;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return 0;
	std::vector<CarWrapper> bots;
	for (CarWrapper car : server.GetCars())
	{
		if (!car)
			continue;
		PriWrapper pri = car.GetPRI();
		if (pri && pri.GetbBot() && car.GetOwnerName().rfind(namePrefix, 0) == 0)
			bots.push_back(car);
	}
	int removed = 0;
	for (CarWrapper& car : bots)
	{
		botInputs_.erase(carId(car));
		AIControllerWrapper ai = car.GetAIController();
		if (!ai)
			continue;
		server.RemovePlayer(ai);
		++removed;
	}
	return removed;
}

bool BakkesGameAdapter::setCarState(uint64_t id, const CarTarget& target)
{
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	for (CarWrapper car : server.GetCars())
	{
		if (!car || carId(car) != id)
			continue;
		car.SetLocation(toVector(target.location));
		car.SetVelocity(toVector(target.velocity));
		car.SetRotation(toRotator(target.rotation));
		car.SetAngularVelocity(toVector(target.angularVelocity), false);
		if (target.boost)
		{
			if (BoostWrapper boost = car.GetBoostComponent())
				boost.SetBoostAmount(std::clamp(*target.boost, 0.f, 100.f) / 100.f * boost.GetMaxBoostAmount());
		}
		return true;
	}
	return false;
}

void BakkesGameAdapter::setBotInput(uint64_t id, std::optional<CarInput> input)
{
	if (input)
		botInputs_[id] = *input;
	else
		botInputs_.erase(id);
}

bool BakkesGameAdapter::respawnBall()
{
	if (!gw_->IsInFreeplay())
		return false;
	ServerWrapper server = gw_->GetCurrentGameState();
	if (!server)
		return false;
	server.ResetBalls();
	return true;
}

void BakkesGameAdapter::runOnGameThread(std::function<void()> fn)
{
	gw_->Execute([fn = std::move(fn)](GameWrapper*) { fn(); });
}

void BakkesGameAdapter::runAfter(float seconds, std::function<void()> fn)
{
	gw_->SetTimeout([fn = std::move(fn)](GameWrapper*) { fn(); }, seconds);
}

std::filesystem::path BakkesGameAdapter::dataFolder()
{
	return gw_->GetDataFolder();
}

int BakkesGameAdapter::loadImage(const std::filesystem::path& file)
{
	std::error_code ec;
	if (!std::filesystem::is_regular_file(file, ec))
		return -1;
	const auto written = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
	const std::string key = file.string() + "|" + std::to_string(written);
	std::lock_guard lock(imagesMutex_);
	if (const auto it = imageHandles_.find(key); it != imageHandles_.end())
		return it->second;
	images_.push_back(std::make_shared<ImageWrapper>(file, false, true));
	const int handle = static_cast<int>(images_.size()) - 1;
	imageHandles_[key] = handle;
	return handle;
}

ImageInfo BakkesGameAdapter::image(int handle)
{
	std::shared_ptr<ImageWrapper> img;
	{
		std::lock_guard lock(imagesMutex_);
		if (handle < 0 || handle >= static_cast<int>(images_.size()))
			return {};
		img = images_[handle];
	}
	if (!img)
		return {};
	void* texture = img->GetImGuiTex(); // loads on first use (render thread)
	if (!texture)
		return {};
	const Vector2F size = img->GetSizeF();
	return {texture, size.X, size.Y};
}
