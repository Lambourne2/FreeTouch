#include "pch.h"
#include "objects/ObjectSystem.h"

#include <algorithm>
#include <cmath>

using nlohmann::json;

namespace
{
	constexpr float kBallRadius = 92.75f;
	constexpr float kPi = 3.14159265f;
	constexpr float kBlockerBounce = 0.6f;    // like a wall
	constexpr uint64_t kBlockerEventGapTicks = 15; // one blocker "hit" per contact, not one per tick

	Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
	Vec3 normalized(const Vec3& v, const Vec3& fallback = {0, 0, 1})
	{
		const float len = v.length();
		return len > 1e-4f ? v * (1.f / len) : fallback;
	}

	// Two axes across a flat object facing `n`: `side` (horizontal when possible) and `up`.
	void basis(const Vec3& n, Vec3& side, Vec3& up)
	{
		const Vec3 hint = std::fabs(n.z) > 0.9f ? Vec3(1, 0, 0) : Vec3(0, 0, 1);
		side = normalized(cross(hint, n), {1, 0, 0});
		up = cross(n, side);
	}

	Vec3 readVec(const json& v, Vec3 fallback)
	{
		if (!v.is_array() || v.size() != 3)
			return fallback;
		return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
	}

	Rgba colorNamed(const std::string& name)
	{
		if (name == "blue") return {63, 140, 255, 255};
		if (name == "orange") return {255, 138, 42, 255};
		if (name == "green") return {90, 220, 110, 255};
		if (name == "red") return {255, 80, 80, 255};
		if (name == "white") return {240, 244, 250, 255};
		if (name == "grey") return {150, 156, 168, 255}; // a closed net (like the training packs' closed tiles)
		return {43, 217, 196, 255}; // accent
	}

	// Where the segment p0 -> p1 crosses the plane through `c` with normal `n`, offset by `offset` along n.
	// Returns the fraction along the segment, or -1 if it does not cross. `fromFront` limits it to crossings from the
	// side `n` points to.
	float crossing(const Vec3& p0, const Vec3& p1, const Vec3& c, const Vec3& n, float offset, bool fromFront)
	{
		const float s0 = (p0 - c).dot(n) - offset;
		const float s1 = (p1 - c).dot(n) - offset;
		if (fromFront ? !(s0 > 0.f && s1 <= 0.f) : !(s0 * s1 < 0.f || (s0 != 0.f && s1 == 0.f)))
			return -1.f;
		return s0 / (s0 - s1);
	}

	// Distance from `p` to segment a-b in the floor plane (z ignored).
	float distance2d(const Vec3& p, const Vec3& a, const Vec3& b)
	{
		const float abx = b.x - a.x, aby = b.y - a.y;
		const float len2 = abx * abx + aby * aby;
		float t = len2 > 0.f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2 : 0.f;
		t = std::clamp(t, 0.f, 1.f);
		const float dx = p.x - (a.x + abx * t), dy = p.y - (a.y + aby * t);
		return std::sqrt(dx * dx + dy * dy);
	}

	constexpr float kMinLineWidth = 2.f; // objects stay visible from across the field

	// Clips a-b to the inside of the arena (floor, ceiling, side walls) so objects that reach into a wall or the
	// floor look embedded instead of showing through. False if nothing is left.
	bool clipToArena(Vec3& a, Vec3& b)
	{
		float t0 = 0.f, t1 = 1.f;
		const Vec3 d = b - a;
		// planes: x >= -4096, x <= 4096, z >= 0, z <= 2044 (as p * t <= q)
		const float p[4] = {-d.x, d.x, -d.z, d.z};
		const float q[4] = {a.x + 4096.f, 4096.f - a.x, a.z, 2044.f - a.z};
		for (int i = 0; i < 4; ++i)
		{
			if (p[i] == 0.f)
			{
				if (q[i] < 0.f)
					return false;
				continue;
			}
			const float r = q[i] / p[i];
			if (p[i] < 0.f) t0 = std::max(t0, r);
			else t1 = std::min(t1, r);
			if (t0 > t1)
				return false;
		}
		const Vec3 start = a + d * t0, end = a + d * t1;
		a = start;
		b = end;
		return true;
	}

	void line(Canvas& c, Vec3 a, Vec3 b, Rgba col, float width)
	{
		if (clipToArena(a, b))
			c.line3d(a, b, col, width, kMinLineWidth);
	}

	std::vector<Vec3> circlePoints(const Vec3& centre, const Vec3& n, float r, int segments)
	{
		Vec3 side, up;
		basis(n, side, up);
		std::vector<Vec3> pts;
		for (int i = 0; i < segments; ++i)
		{
			const float a = 2.f * kPi * i / segments;
			pts.push_back(centre + side * (r * std::cos(a)) + up * (r * std::sin(a)));
		}
		return pts;
	}

	void circle3d(Canvas& c, const Vec3& centre, const Vec3& n, float r, Rgba col, float width, int segments = 40,
		float minWidth = kMinLineWidth)
	{
		const auto pts = circlePoints(centre, n, r, segments);
		for (int i = 0; i < segments; ++i)
		{
			Vec3 a = pts[i], b = pts[(i + 1) % segments];
			if (clipToArena(a, b))
				c.line3d(a, b, col, width, minWidth);
		}
	}

	// A darker shade (instead of alpha, which the canvas does not honour reliably).
	Rgba scaled(Rgba c, float k)
	{
		auto f = [k](uint8_t v) { return static_cast<uint8_t>(std::clamp(v * k, 0.f, 255.f)); };
		return {f(c.r), f(c.g), f(c.b), c.a};
	}

	// Stripes across a rectangle (centre `f`, half-axes `hs` and `hu`) every `spacing` uu: horizontal, or diagonal
	// (45 degrees, clipped to the rectangle). Canvas fills are opaque, so faces are shown with see-through hatching.
	void hatch(Canvas& c, const Vec3& f, const Vec3& hs, const Vec3& hu, float spacing, bool diagonal, Rgba col, float width)
	{
		const float w = hs.length() * 2.f, h = hu.length() * 2.f;
		if (w <= 0.f || h <= 0.f)
			return;
		const Vec3 sx = hs * (1.f / hs.length()), sy = hu * (1.f / hu.length());
		auto at = [&](float x, float y) { return f + sx * (x - w * 0.5f) + sy * (y - h * 0.5f); };
		if (!diagonal)
		{
			for (float y = spacing * 0.5f; y < h; y += spacing)
				line(c, at(0.f, y), at(w, y), col, width);
			return;
		}
		// Lines x - y = k for k from -h to w, clipped to [0,w] x [0,h].
		for (float k = -h + spacing * 0.5f; k < w; k += spacing)
		{
			const float x0 = std::max(0.f, k), y0 = x0 - k;
			const float x1 = std::min(w, k + h), y1 = x1 - k;
			if (y0 >= 0.f && y0 <= h && y1 >= 0.f && y1 <= h && x1 > x0)
				line(c, at(x0, y0), at(x1, y1), col, width);
		}
	}
}

const char* objects::typeName(ObjectType type)
{
	switch (type)
	{
	case ObjectType::Target: return "target";
	case ObjectType::Blocker: return "blocker";
	case ObjectType::Ring: return "ring";
	case ObjectType::Pin: return "pin";
	case ObjectType::Checkpoint: return "checkpoint";
	case ObjectType::Lane: return "lane";
	case ObjectType::Panel: return "panel";
	case ObjectType::Circle: return "circle";
	default: return "unknown";
	}
}

std::vector<WorldObject> objects::fromJson(const json& list, std::vector<std::string>& warnings)
{
	std::vector<WorldObject> out;
	if (!list.is_array())
		return out;
	int index = 0;
	for (const auto& j : list)
	{
		WorldObject o;
		const std::string type = j.value("type", "");
		o.id = j.value("id", type + "_" + std::to_string(index++));
		if (type == "target") o.type = ObjectType::Target;
		else if (type == "blocker") o.type = ObjectType::Blocker;
		else if (type == "ring") o.type = ObjectType::Ring;
		else if (type == "pin") o.type = ObjectType::Pin;
		else if (type == "checkpoint") o.type = ObjectType::Checkpoint;
		else if (type == "lane") o.type = ObjectType::Lane;
		else if (type == "panel") o.type = ObjectType::Panel;
		else if (type == "circle") o.type = ObjectType::Circle;
		else
		{
			warnings.push_back("object '" + o.id + "': type '" + type + "' is not supported yet");
			continue;
		}

		// Per-type defaults, then what the scenario says.
		switch (o.type)
		{
		case ObjectType::Ring: o.radius = 300.f; break;
		case ObjectType::Checkpoint: o.radius = 400.f; o.trigger = Trigger::Car; break;
		case ObjectType::Circle: o.radius = 250.f; o.trigger = Trigger::Car; o.normal = {0, 0, 1}; break;
		case ObjectType::Lane: o.trigger = Trigger::Car; break;
		case ObjectType::Pin: o.radius = 50.f; o.height = 230.f; o.color = colorNamed("white"); break;
		case ObjectType::Blocker: o.color = colorNamed("blue"); break;
		default: break;
		}
		o.location = readVec(j.value("location", json()), o.location);
		o.normal = normalized(readVec(j.value("normal", json()), o.normal),
			o.type == ObjectType::Ring || o.type == ObjectType::Checkpoint ? Vec3(0, 1, 0) : Vec3(0, 0, 1));
		o.size = readVec(j.value("size", json()), o.size);
		o.rect = o.type == ObjectType::Target && j.contains("size");
		o.radius = j.value("radius", o.radius);
		o.width = j.value("width", o.width);
		o.withinUu = j.value("withinUu", o.withinUu);
		o.order = j.value("order", o.order);
		if (j.contains("color"))
			o.color = colorNamed(j["color"].get<std::string>());
		if (j.contains("triggeredBy"))
		{
			const std::string t = j["triggeredBy"].get<std::string>();
			o.trigger = t == "car" ? Trigger::Car : t == "car_with_ball" ? Trigger::CarWithBall : Trigger::Ball;
		}
		if (j.contains("points"))
		{
			for (const auto& p : j["points"])
				o.points.push_back(readVec(p, {}));
		}
		if (o.type == ObjectType::Lane && o.points.size() < 2)
		{
			warnings.push_back("lane '" + o.id + "' needs at least 2 points");
			continue;
		}
		out.push_back(std::move(o));
	}
	return out;
}

void ObjectSystem::setObjects(std::vector<WorldObject> objects)
{
	objects_ = std::move(objects);
	prevBall_.reset();
	prevCar_.reset();
	onCourse_ = false; // leaving counts only after the car has been on the course
	lastBlockerTick_.assign(objects_.size(), 0);
}

void ObjectSystem::clear()
{
	setObjects({});
}

void ObjectSystem::setHit(const std::string& id)
{
	for (auto& o : objects_)
	{
		if (o.id != id)
			continue;
		o.hit = true;
		if (o.type == ObjectType::Pin || o.type == ObjectType::Panel)
			o.active = false;
	}
}

bool ObjectSystem::allHit() const
{
	bool any = false;
	for (const auto& o : objects_)
	{
		if (!objects::isGoal(o))
			continue;
		any = true;
		if (!o.hit)
			return false;
	}
	return any;
}

int ObjectSystem::nextInOrder() const
{
	int next = -1;
	for (const auto& o : objects_)
	{
		if (o.order >= 0 && !o.hit && (next < 0 || o.order < next))
			next = o.order;
	}
	return next;
}

std::optional<Vec3> ObjectSystem::objectiveMarker(int ahead) const
{
	const WorldObject* target = nullptr;
	if (nextInOrder() >= 0)
	{
		// The course's remaining objectives by order; mark `ahead` past the next (or the last one).
		std::vector<const WorldObject*> left;
		for (const auto& o : objects_)
		{
			if (o.order >= 0 && !o.hit)
				left.push_back(&o);
		}
		std::sort(left.begin(), left.end(), [](const WorldObject* a, const WorldObject* b) { return a->order < b->order; });
		target = left[std::min(static_cast<size_t>(std::max(ahead, 0)), left.size() - 1)];
	}
	else
	{
		for (const auto& o : objects_)
		{
			if (objects::isGoal(o) && !o.hit && o.active)
			{
				target = &o;
				break;
			}
		}
	}
	if (!target)
		return std::nullopt;
	switch (target->type)
	{
	case ObjectType::Ring:
	case ObjectType::Checkpoint:
	{
		// Just above the top of the ring (in its own plane), so the car does not run into the hidden ball.
		Vec3 side, up;
		basis(target->normal, side, up);
		if (up.z < 0.f)
			up = up * -1.f;
		return target->location + up * (target->radius + 60.f);
	}
	case ObjectType::Circle:
		return target->location + target->normal * 20.f; // on the floor, a wall or the ceiling
	case ObjectType::Pin:
		return target->location + Vec3(0.f, 0.f, target->height);
	default:
		return target->location;
	}
}

ObjectSystem::TickResult ObjectSystem::tick(const Snapshot& snap)
{
	TickResult out;
	tick_ = snap.tick;
	const BallState* ball = snap.ball ? &*snap.ball : nullptr;
	const CarState* car = snap.localCar();
	// Lanes form one course: the car is on it while inside any lane. Leaving counts only with the wheels down
	// outside every lane (jumping a gap between lanes is fine), and only after the car has been on the course.
	if (car)
	{
		bool anyLane = false, inside = false;
		const WorldObject* nearest = nullptr;
		float nearestD = 1e9f;
		for (const auto& o : objects_)
		{
			if (o.type != ObjectType::Lane)
				continue;
			anyLane = true;
			for (size_t k = 0; k + 1 < o.points.size(); ++k)
			{
				const float d = distance2d(car->location, o.points[k], o.points[k + 1]);
				inside = inside || d <= o.width * 0.5f;
				if (d < nearestD)
				{
					nearestD = d;
					nearest = &o;
				}
			}
		}
		if (anyLane)
		{
			if (inside)
				onCourse_ = true;
			else if (onCourse_ && car->onGround)
			{
				out.leftLane = nearest ? nearest->id : "lane";
				onCourse_ = false;
			}
		}
	}
	for (size_t i = 0; i < objects_.size(); ++i)
	{
		WorldObject& o = objects_[i];
		if (o.type == ObjectType::Lane)
			continue;
		if (o.type == ObjectType::Blocker)
		{
			if (!ball)
				continue;
			// Ball sphere against the box (axes: side = width, up = height, normal = depth).
			Vec3 side, up;
			basis(o.normal, side, up);
			const Vec3 rel = ball->location - o.location;
			const float lx = std::clamp(rel.dot(side), -o.size.x * 0.5f, o.size.x * 0.5f);
			const float ly = std::clamp(rel.dot(up), -o.size.y * 0.5f, o.size.y * 0.5f);
			const float lz = std::clamp(rel.dot(o.normal), -o.size.z * 0.5f, o.size.z * 0.5f);
			const Vec3 closest = o.location + side * lx + up * ly + o.normal * lz;
			const Vec3 away = ball->location - closest;
			const float dist = away.length();
			if (dist >= kBallRadius)
				continue;
			const Vec3 n = dist > 1e-3f ? away * (1.f / dist) : o.normal;
			BallTarget bounce{closest + n * (kBallRadius + 1.f), ball->velocity, ball->angularVelocity};
			const float vn = ball->velocity.dot(n);
			if (vn < 0.f)
				bounce.velocity = ball->velocity - n * ((1.f + kBlockerBounce) * vn);
			out.ballBounce = bounce;
			if (tick_ >= lastBlockerTick_[i] + kBlockerEventGapTicks)
			{
				out.hits.push_back({o.id, o.type, "ball", ball->location, ball->velocity.length(), true});
				o.hit = true;
			}
			lastBlockerTick_[i] = tick_;
			continue;
		}
		if (!o.active || o.hit)
			continue;
		testObject(o, ball, car, out);
	}
	prevBall_ = ball ? std::optional<Vec3>(ball->location) : std::nullopt;
	prevCar_ = car ? std::optional<Vec3>(car->location) : std::nullopt;
	return out;
}

void ObjectSystem::testObject(WorldObject& o, const BallState* ball, const CarState* car, TickResult& out)
{
	const bool byBall = o.trigger == Trigger::Ball;
	const Vec3* p1 = byBall ? (ball ? &ball->location : nullptr) : (car ? &car->location : nullptr);
	const std::optional<Vec3>& prev = byBall ? prevBall_ : prevCar_;
	if (!p1 || !prev)
		return;
	const Vec3 p0 = *prev;
	const float bodyRadius = byBall ? kBallRadius : 0.f;

	bool hit = false;
	Vec3 at = *p1;
	switch (o.type)
	{
	case ObjectType::Target:
	case ObjectType::Panel:
	{
		// The ball's surface reaching the face from the front, inside the disc / rectangle.
		const float t = crossing(p0, *p1, o.location, o.normal, bodyRadius, true);
		if (t < 0.f)
			break;
		at = p0 + (*p1 - p0) * t;
		const Vec3 rel = at - o.location;
		if (o.type == ObjectType::Target && !o.rect)
		{
			const Vec3 lateral = rel - o.normal * rel.dot(o.normal);
			hit = lateral.length() <= o.radius;
		}
		else
		{
			Vec3 side, up;
			basis(o.normal, side, up);
			hit = std::fabs(rel.dot(side)) <= o.size.x * 0.5f && std::fabs(rel.dot(up)) <= o.size.y * 0.5f;
		}
		break;
	}
	case ObjectType::Ring:
	case ObjectType::Checkpoint:
	{
		// The centre passing through the ring plane (either way) inside the ring.
		const float t = crossing(p0, *p1, o.location, o.normal, 0.f, false);
		if (t < 0.f)
			break;
		at = p0 + (*p1 - p0) * t;
		const Vec3 rel = at - o.location;
		const Vec3 lateral = rel - o.normal * rel.dot(o.normal);
		hit = lateral.length() <= o.radius;
		if (hit && o.trigger == Trigger::CarWithBall)
			hit = ball && (ball->location - *p1).length() <= o.withinUu;
		break;
	}
	case ObjectType::Pin:
	{
		// Ball (or car) against an upright cylinder standing on the floor.
		const float d = distance2d(o.location, p0, *p1);
		const float zMin = o.location.z - bodyRadius, zMax = o.location.z + o.height + bodyRadius;
		hit = d <= o.radius + (byBall ? kBallRadius : 60.f) && p1->z >= zMin && p1->z <= zMax;
		break;
	}
	case ObjectType::Circle:
	{
		// A disc on a surface (floor, wall, ceiling; `normal` points into the arena): the car on or just off it.
		const Vec3 rel = *p1 - o.location;
		const float off = rel.dot(o.normal);
		const Vec3 lateral = rel - o.normal * off;
		hit = lateral.length() <= o.radius && off > -60.f && off < 250.f;
		break;
	}
	default:
		break;
	}
	if (!hit)
		return;

	Hit h{o.id, o.type, byBall ? "ball" : "car", at, ball ? ball->velocity.length() : 0.f, true};
	if (o.order >= 0 && o.order != nextInOrder())
	{
		h.inOrder = false; // reported, but it does not count
		out.hits.push_back(h);
		return;
	}
	o.hit = true;
	if (o.type == ObjectType::Pin || o.type == ObjectType::Panel)
		o.active = false;
	out.hits.push_back(h);
}

void ObjectSystem::draw(Canvas& c, double time) const
{
	const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(time * 4.0));
	const int next = nextInOrder();
	// Route guide: a dashed line from the next course ring to the one after it.
	if (next >= 0)
	{
		const WorldObject* a = nullptr;
		const WorldObject* b = nullptr;
		for (const auto& o : objects_)
		{
			if (o.order == next) a = &o;
			if (o.order == next + 1) b = &o;
		}
		if (a && b)
		{
			const Vec3 d = b->location - a->location;
			const float len = d.length();
			const Vec3 u = d * (1.f / std::max(len, 1.f));
			const float offset = static_cast<float>(std::fmod(time * 600.0, 220.0)); // dashes flow toward b
			for (float s = a->radius * 0.6f + offset - 220.f; s < len - b->radius * 0.6f; s += 220.f)
			{
				const float s0 = std::max(s, a->radius * 0.6f), s1 = std::min(s + 120.f, len - b->radius * 0.6f);
				if (s1 > s0)
					line(c, a->location + u * s0, a->location + u * s1, Rgba{220, 230, 245, 255}, 3.f);
			}
		}
	}
	for (const auto& o : objects_)
	{
		if (!o.active)
			continue;
		Rgba col = o.color;
		if (o.hit && o.type != ObjectType::Blocker)
			col = col.withAlpha(0.3f);
		switch (o.type)
		{
		case ObjectType::Target:
		{
			const Vec3 front = o.location + o.normal * 2.f;
			if (o.rect)
			{
				// The pocket: a glowing translucent fill, a bright frame and a pulsing inner frame.
				Vec3 side, up;
				basis(o.normal, side, up);
				if (!o.hit)
					hatch(c, front, side * (o.size.x * 0.5f), up * (o.size.y * 0.5f), 55.f, true, col, 2.5f);
				auto frame = [&](float inset, Rgba fc, float w) {
					const Vec3 hs = side * (o.size.x * 0.5f - inset), hu = up * (o.size.y * 0.5f - inset);
					const Vec3 q[4] = {front - hs - hu, front + hs - hu, front + hs + hu, front - hs + hu};
					for (int k = 0; k < 4; ++k)
						line(c, q[k], q[(k + 1) % 4], fc, w);
				};
				frame(0.f, col, 5.f);
				if (!o.hit)
					frame(18.f + 22.f * pulse, col.withAlpha(0.55f), 3.f);
				break;
			}
			if (!o.hit)
				circle3d(c, front, o.normal, o.radius * (0.3f + 0.6f * pulse), col, 2.5f, 28);
			circle3d(c, front, o.normal, o.radius, col, 4.f);
			circle3d(c, front, o.normal, o.radius * 0.62f, col.withAlpha(0.8f), 3.f);
			circle3d(c, front, o.normal, o.radius * 0.25f, col, 3.f, 20);
			break;
		}
		case ObjectType::Ring:
		case ObjectType::Checkpoint:
		{
			if (o.hit)
				break; // passed: gone
			// Course rings by how soon they come: the next one bold and pulsing, then the one after, then the rest
			// thin and darker (canvas alpha is unreliable, so dimmer colours are darker ones).
			const int rank = o.order >= 0 && next >= 0 ? o.order - next : 0;
			if (rank == 0)
			{
				const Rgba bright = scaled(col, 0.85f + 0.15f * pulse);
				circle3d(c, o.location, o.normal, o.radius, bright, 7.f + 2.f * pulse, 48, 3.f);
				circle3d(c, o.location, o.normal, o.radius - 16.f, bright, 5.f, 48, 3.f);
				circle3d(c, o.location, o.normal, o.radius - 32.f, scaled(col, 0.7f), 3.f, 48, 2.f);
				// Ticks around the rim so it reads even edge-on.
				Vec3 side, up;
				basis(o.normal, side, up);
				for (int k = 0; k < 8; ++k)
				{
					const float a = kPi * 0.25f * k;
					const Vec3 dir = side * std::cos(a) + up * std::sin(a);
					line(c, o.location + dir * (o.radius + 10.f), o.location + dir * (o.radius + 60.f), bright, 5.f);
				}
			}
			else if (rank == 1)
			{
				circle3d(c, o.location, o.normal, o.radius, scaled(col, 0.7f), 5.f, 40, 2.5f);
				circle3d(c, o.location, o.normal, o.radius - 16.f, scaled(col, 0.55f), 3.f, 40, 2.f);
			}
			else
			{
				circle3d(c, o.location, o.normal, o.radius, scaled(col, 0.45f), 3.f, 32, 2.f);
			}
			break;
		}
		case ObjectType::Panel:
		case ObjectType::Blocker:
		{
			Vec3 side, up;
			basis(o.normal, side, up);
			const Vec3 hs = side * (o.size.x * 0.5f), hu = up * (o.size.y * 0.5f);
			const float depth = o.type == ObjectType::Blocker ? o.size.z * 0.5f : 2.f;
			const Vec3 f = o.location + o.normal * depth; // front face
			const Vec3 corners[4] = {f - hs - hu, f + hs - hu, f + hs + hu, f - hs + hu};
			hatch(c, f, hs, hu, o.type == ObjectType::Blocker ? 70.f : 45.f, o.type == ObjectType::Panel, col, 2.f);
			for (int k = 0; k < 4; ++k)
				line(c, corners[k], corners[(k + 1) % 4], col, 4.f);
			break;
		}
		case ObjectType::Pin:
		{
			const Vec3 base = o.location + Vec3(0, 0, 2);
			const Vec3 top = base + Vec3(0, 0, o.height);
			circle3d(c, base, {0, 0, 1}, o.radius * 0.7f, col, 3.f, 16);
			circle3d(c, base + Vec3(0, 0, o.height * 0.35f), {0, 0, 1}, o.radius, col, 3.f, 16);
			circle3d(c, base + Vec3(0, 0, o.height * 0.75f), {0, 0, 1}, o.radius * 0.55f, col, 3.f, 16);
			for (int k = 0; k < 4; ++k)
			{
				const float a = kPi * 0.5f * k;
				const Vec3 dir(std::cos(a), std::sin(a), 0.f);
				line(c, base + dir * (o.radius * 0.7f), base + Vec3(0, 0, o.height * 0.35f) + dir * o.radius, col, 3.f);
				line(c, base + Vec3(0, 0, o.height * 0.35f) + dir * o.radius, top + dir * (o.radius * 0.3f), col, 3.f);
			}
			break;
		}
		case ObjectType::Circle:
		{
			const Vec3 face = o.location + o.normal * 4.f;
			circle3d(c, face, o.normal, o.radius, col, 4.f);
			circle3d(c, face, o.normal, o.radius * 0.8f, col, 2.f);
			if (!o.hit)
				circle3d(c, face, o.normal, o.radius * (0.35f + 0.5f * pulse), col.withAlpha(0.5f), 2.f);
			break;
		}
		case ObjectType::Lane:
		{
			// Both edges on the floor.
			for (size_t k = 0; k + 1 < o.points.size(); ++k)
			{
				const Vec3 a = o.points[k], b = o.points[k + 1];
				const Vec3 dir = normalized(Vec3(b.x - a.x, b.y - a.y, 0.f), {1, 0, 0});
				const Vec3 off = Vec3(-dir.y, dir.x, 0.f) * (o.width * 0.5f);
				const Vec3 lift(0, 0, 4);
				line(c, a + off + lift, b + off + lift, col, 4.f);
				line(c, a - off + lift, b - off + lift, col, 4.f);
			}
			break;
		}
		default:
			break;
		}
	}
}
