#pragma once

#include <functional>
#include <string>

class Controls;

// Bindings screen: shows each exercise action with its key; Rebind / Default buttons. Render thread, pure ImGui:
// it only reads Controls and hands requests to the callbacks, which run the real work on the game thread.
class BindingsScreen
{
public:
	struct Callbacks
	{
		std::function<void(const std::string& actionId)> rebind;
		std::function<void(const std::string& actionId)> resetToDefault;
		std::function<void()> cancelCapture;
		std::function<bool()> exerciseBindsActive;
	};

	BindingsScreen(const Controls& controls, Callbacks callbacks) : controls_(controls), cb_(std::move(callbacks)) {}

	// `width`: the space to fill (0 = what's left on the line).
	void render(float width = 0.f);

private:
	const Controls& controls_;
	Callbacks cb_;
};
