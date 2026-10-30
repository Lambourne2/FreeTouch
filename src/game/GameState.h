#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Plain game-state types. No SDK types here: everything outside src/game/ sees only these.
// Units: Unreal units (uu), uu/s, rotations in Unreal rotation units (65536 = 360 degrees), boost 0..100.

struct Vec3
{
	float x = 0.f, y = 0.f, z = 0.f;

	Vec3() = default;
	Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

	Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
	float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
	float length() const { return std::sqrt(dot(*this)); }
};

struct Rot
{
	int pitch = 0, yaw = 0, roll = 0;
};

struct BallState
{
	Vec3 location;
	Vec3 velocity;
	Vec3 angularVelocity;
};

struct CarState
{
	// Stable for the player across freeplay resets (the car object itself is replaced on every reset).
	uint64_t id = 0;
	std::string name;
	int team = 0;
	bool isLocal = false;
	bool isBot = false;
	int body = 0;           // car body product id (23 = Octane; 0 = unknown)

	Vec3 location;
	Vec3 velocity;
	Vec3 angularVelocity;
	Rot rotation;
	float boost = 0.f;  // 0..100

	bool hasFlip = false;
	bool onGround = false;
	bool onWall = false;
	bool jumped = false;
	bool doubleJumped = false;
	// Wheels touching anything / touching the world (floor, walls, ceiling); -1 = not read. More wheels touching than
	// world wheels means wheels on the ball or a car (a flip reset). To verify in game (logged with flip resets).
	int wheelContacts = -1;
	int worldWheelContacts = -1;
};

// A car's controls for one tick: what the player pressed, or what a bot is told to do.
struct CarInput
{
	float throttle = 0.f, steer = 0.f, pitch = 0.f, yaw = 0.f, roll = 0.f;
	bool jump = false, boost = false, handbrake = false;
};

struct Snapshot
{
	uint64_t tick = 0;     // physics ticks since the plugin loaded (120 per second)
	double time = 0.0;     // tick / 120
	bool inFreeplay = false;
	bool inReplay = false;
	bool paused = false;

	std::optional<BallState> ball;
	std::vector<CarState> cars;
	int localCarIndex = -1;
	std::optional<CarInput> localInput; // what the player is pressing this tick (tick snapshots only)
	// Boost pads that can be picked up, in core/BoostPads.h's order (from pickup events; all true until one is seen).
	std::array<bool, 34> padAvailable = [] { std::array<bool, 34> a{}; a.fill(true); return a; }();

	const CarState* localCar() const
	{
		return localCarIndex >= 0 && localCarIndex < static_cast<int>(cars.size()) ? &cars[localCarIndex] : nullptr;
	}
};

struct TouchEvent
{
	uint64_t tick = 0;
	uint64_t carId = 0;
	std::string carName;
	bool isLocal = false;
	bool isBot = false;
	// The toucher was simulated (sent vehicle input) in the last 2 ticks. False for the goal-replay copy of a car,
	// which re-fires the scoring touch with the same player id. Detectors must ignore touches where this is false.
	bool simulated = false;
	Vec3 carLocation;
	Vec3 ballLocation;
	Vec3 ballVelocity;     // at the touch; the hit is not applied yet (it shows up over the next 2 ticks)
};

// What a scenario asks for. Optional fields are left as they are.
struct BallTarget
{
	Vec3 location;
	Vec3 velocity;
	Vec3 angularVelocity;
};

// The player's rank in one ranked playlist, as the game has it cached (no network call by the plugin).
struct MmrReading
{
	bool synced = false;   // the game has this playlist's rating for the player
	float mmr = 0.f;
	int tier = 0;          // 0 unranked, 1 Bronze I ... 22 Supersonic Legend
	int division = 0;      // 0..3
	int matches = 0;
};

// The replay being watched.
struct ReplayInfo
{
	bool active = false;
	std::string name;       // the replay's name ("" when it has none)
	std::string map;        // its arena, e.g. Stadium_P
	double time = 0.0;      // seconds into the replay
	int frame = 0;
	int frames = 0;
	uint64_t focusCarId = 0; // the car the camera follows (CarState::id), 0 = none / free camera
};

// The custom training pack being played (active false outside custom training). Name and code are read when the
// round changes; type is the game's training type (logged, not relied on).
struct TrainingInfo
{
	bool active = false;
	std::string name;     // the pack's name ("" if unknown)
	std::string code;     // its share code, e.g. "A503-264B-7D5E-1C3B" ("" for an unshared pack)
	std::string creator;
	int type = -1;
	int round = -1;       // the shot being played, 0-based
	int rounds = 0;       // shots in the pack
	double secondsLeft = 0.0; // the round clock (while the shot waits to start: its time limit)
	// The shot's launch as the game draws its path while it waits (Ball_TA trajectory): readable before it starts.
	Vec3 trajectoryVelocity;
	Vec3 trajectoryLocation;
	bool drawsTrajectory = false;
};

// A dashboard image as the UI sees it: an ImGui texture (null until loaded) and its pixel size.
struct ImageInfo
{
	void* texture = nullptr;
	float width = 0.f;
	float height = 0.f;
};

struct CarTarget
{
	Vec3 location;
	Rot rotation;
	Vec3 velocity;
	Vec3 angularVelocity;
	std::optional<float> boost;  // 0..100
};
