#include "pch.h"
#include "ui/Icons.h"

#include <cmath>

namespace
{
	ImVec2 at(ImVec2 c, float s, float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); }
}

void icons::draw(ImDrawList* dl, Icon icon, ImVec2 c, float size, ImU32 col)
{
	const float s = size * 0.5f; // coordinates below are in [-1, 1]
	const float t = std::max(1.5f, size * 0.09f); // stroke
	switch (icon)
	{
	case Icon::Home:
		dl->AddTriangleFilled(at(c, s, -0.9f, -0.05f), at(c, s, 0.f, -0.85f), at(c, s, 0.9f, -0.05f), col);
		dl->AddRectFilled(at(c, s, -0.6f, -0.1f), at(c, s, 0.6f, 0.8f), col, 1.f);
		dl->AddRectFilled(at(c, s, -0.15f, 0.25f), at(c, s, 0.15f, 0.8f), IM_COL32(0, 0, 0, 140));
		break;
	case Icon::Sessions: // stacked layers
		for (int i = 0; i < 3; ++i)
		{
			const float y = -0.55f + i * 0.5f;
			dl->AddRectFilled(at(c, s, -0.8f, y - 0.14f), at(c, s, 0.8f, y + 0.14f), col, 2.f);
		}
		break;
	case Icon::Exercises: // 2x2 grid
		for (int i = 0; i < 4; ++i)
		{
			const float x = (i % 2) ? 0.1f : -0.8f;
			const float y = (i / 2) ? 0.1f : -0.8f;
			dl->AddRectFilled(at(c, s, x, y), at(c, s, x + 0.7f, y + 0.7f), col, 2.f);
		}
		break;
	case Icon::QuickPlay: // lightning bolt: two triangles
		dl->AddTriangleFilled(at(c, s, 0.25f, -0.95f), at(c, s, -0.55f, 0.15f), at(c, s, 0.1f, 0.15f), col);
		dl->AddTriangleFilled(at(c, s, -0.1f, -0.15f), at(c, s, 0.55f, -0.15f), at(c, s, -0.25f, 0.95f), col);
		break;
	case Icon::Races: // flag
		dl->AddLine(at(c, s, -0.6f, -0.85f), at(c, s, -0.6f, 0.9f), col, t);
		dl->AddRectFilled(at(c, s, -0.6f, -0.85f), at(c, s, 0.75f, -0.05f), col, 1.f);
		break;
	case Icon::Replays: // circle with play triangle
		dl->AddCircle(c, s * 0.85f, col, 24, t);
		dl->AddTriangleFilled(at(c, s, -0.25f, -0.4f), at(c, s, 0.45f, 0.f), at(c, s, -0.25f, 0.4f), col);
		break;
	case Icon::Stats: // bars
		dl->AddRectFilled(at(c, s, -0.8f, 0.1f), at(c, s, -0.35f, 0.85f), col, 1.f);
		dl->AddRectFilled(at(c, s, -0.22f, -0.35f), at(c, s, 0.22f, 0.85f), col, 1.f);
		dl->AddRectFilled(at(c, s, 0.35f, -0.8f), at(c, s, 0.8f, 0.85f), col, 1.f);
		break;
	case Icon::Settings: // gear
	{
		for (int i = 0; i < 8; ++i)
		{
			const float a = i * 3.14159265f / 4.f;
			const float ca = std::cos(a), sa = std::sin(a);
			dl->AddLine(ImVec2(c.x + ca * s * 0.45f, c.y + sa * s * 0.45f), ImVec2(c.x + ca * s * 0.95f, c.y + sa * s * 0.95f), col, t * 1.6f);
		}
		dl->AddCircleFilled(c, s * 0.62f, col, 20);
		dl->AddCircleFilled(c, s * 0.26f, IM_COL32(0, 0, 0, 160), 16);
		break;
	}
	case Icon::Play:
		dl->AddTriangleFilled(at(c, s, -0.5f, -0.75f), at(c, s, 0.75f, 0.f), at(c, s, -0.5f, 0.75f), col);
		break;
	case Icon::Stop:
		dl->AddRectFilled(at(c, s, -0.6f, -0.6f), at(c, s, 0.6f, 0.6f), col, 2.f);
		break;
	case Icon::Star:
		star(dl, c, s * 0.95f, col, true);
		break;
	case Icon::Packs: // a fanned deck of cards with a ball on the front one
		dl->AddRect(at(c, s, -0.85f, -0.55f), at(c, s, 0.25f, 0.75f), col, 2.f, 0, t * 0.8f);
		dl->AddRect(at(c, s, -0.55f, -0.75f), at(c, s, 0.55f, 0.55f), col, 2.f, 0, t * 0.8f);
		dl->AddRectFilled(at(c, s, -0.2f, -0.9f), at(c, s, 0.9f, 0.4f), col, 2.f);
		dl->AddCircleFilled(at(c, s, 0.35f, -0.25f), s * 0.24f, IM_COL32(0, 0, 0, 170), 16);
		break;
	case Icon::Bookmark: // a bookmark ribbon (Saved Shots) with a small ball in it
	{
		const ImVec2 p[] = {at(c, s, -0.6f, -0.85f), at(c, s, 0.6f, -0.85f), at(c, s, 0.6f, 0.9f), at(c, s, 0.f, 0.42f), at(c, s, -0.6f, 0.9f)};
		dl->AddPolyline(p, 5, col, true, t);
		dl->AddCircleFilled(at(c, s, 0.f, -0.25f), s * 0.26f, col, 16);
		break;
	}
	case Icon::Calendar: // a calendar page (the Daily Challenge): frame, a solid top bar, two rings and today's square
		dl->AddRect(at(c, s, -0.85f, -0.6f), at(c, s, 0.85f, 0.85f), col, 2.f, 0, t);
		dl->AddRectFilled(at(c, s, -0.85f, -0.6f), at(c, s, 0.85f, -0.2f), col, 2.f);
		dl->AddRectFilled(at(c, s, -0.5f, -0.9f), at(c, s, -0.3f, -0.45f), col, 1.f);
		dl->AddRectFilled(at(c, s, 0.3f, -0.9f), at(c, s, 0.5f, -0.45f), col, 1.f);
		dl->AddRectFilled(at(c, s, 0.1f, 0.15f), at(c, s, 0.55f, 0.55f), col, 1.f);
		break;
	case Icon::Trophy: // a cup on a stand (Compete)
		dl->AddRectFilled(at(c, s, -0.55f, -0.85f), at(c, s, 0.55f, -0.15f), col, 4.f);
		dl->AddCircleFilled(at(c, s, 0.f, -0.2f), s * 0.55f, col, 20);
		dl->AddCircle(at(c, s, -0.62f, -0.48f), s * 0.24f, col, 12, t);
		dl->AddCircle(at(c, s, 0.62f, -0.48f), s * 0.24f, col, 12, t);
		dl->AddRectFilled(at(c, s, -0.12f, 0.3f), at(c, s, 0.12f, 0.62f), col);
		dl->AddRectFilled(at(c, s, -0.5f, 0.62f), at(c, s, 0.5f, 0.88f), col, 2.f);
		break;
	case Icon::Heart: // supporters: two lobes and a point
		dl->AddCircleFilled(at(c, s, -0.42f, -0.3f), s * 0.48f, col, 20);
		dl->AddCircleFilled(at(c, s, 0.42f, -0.3f), s * 0.48f, col, 20);
		dl->AddTriangleFilled(at(c, s, -0.88f, -0.12f), at(c, s, 0.88f, -0.12f), at(c, s, 0.f, 0.9f), col);
		break;
	case Icon::Record: // a record button (Create): a ring round a dot
		dl->AddCircle(c, s * 0.85f, col, 24, t);
		dl->AddCircleFilled(c, s * 0.45f, col, 20);
		break;
	case Icon::Friends: // two people: a head and shoulders in front, another behind it
		dl->AddCircle(at(c, s, 0.42f, -0.42f), s * 0.24f, col, 16, t);
		dl->PathArcTo(at(c, s, 0.42f, 0.55f), s * 0.42f, 3.14159265f * 1.05f, 3.14159265f * 1.95f, 16);
		dl->PathStroke(col, false, t);
		dl->AddCircleFilled(at(c, s, -0.25f, -0.3f), s * 0.3f, col, 16);
		dl->PathArcTo(at(c, s, -0.25f, 0.9f), s * 0.6f, 3.14159265f, 3.14159265f * 2.f, 20);
		dl->PathFillConvex(col);
		break;
	}
}

void icons::star(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled, float thickness)
{
	ImVec2 p[10];
	for (int i = 0; i < 10; ++i)
	{
		const float a = -1.5707963f + i * 0.6283185f; // from the top, every 36 degrees
		const float rr = i % 2 == 0 ? r : r * 0.45f;
		p[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
	}
	if (filled)
	{
		// The centre pentagon plus a triangle per point (the outline is not convex).
		ImVec2 inner[5] = {p[1], p[3], p[5], p[7], p[9]};
		dl->AddConvexPolyFilled(inner, 5, col);
		for (int i = 0; i < 10; i += 2)
			dl->AddTriangleFilled(p[(i + 9) % 10], p[i], p[(i + 1) % 10], col);
	}
	else
	{
		dl->AddPolyline(p, 10, col, true, thickness);
	}
}
