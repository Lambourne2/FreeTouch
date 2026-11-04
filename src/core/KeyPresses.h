#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

// Turns "the keys down now" into "the keys pressed since the last look". A key already down at the first look counts
// once it has been let go.
class KeyPresses
{
public:
	std::vector<std::string> update(const std::vector<std::string>& down)
	{
		std::set<std::string> now(down.begin(), down.end());
		std::vector<std::string> pressed;
		if (started_)
		{
			for (const auto& key : now)
				if (!held_.count(key))
					pressed.push_back(key);
		}
		started_ = true;
		held_ = std::move(now);
		return pressed;
	}

private:
	bool started_ = false;
	std::set<std::string> held_;
};
