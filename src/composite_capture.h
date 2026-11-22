#pragma once

#include <cstddef>
#include <cstdint>

namespace f4sr
{
	// Only mandatory writes advance the transaction. Conditional passes (6/7)
	// are observations, not prerequisites. No progress carries across frames.
	struct CompositeCapture
	{
		static constexpr int path[]{ 0, 2, 3, 8, 9, 10, 5, 4 };
		static constexpr unsigned maxAttempts = 3;
		std::uint32_t frame{};
		unsigned attempts{};
		std::size_t next{};
		bool active{}, complete{};

		bool canBegin(std::uint32_t id) const
		{
			return !complete && attempts < maxAttempts && (!attempts || frame != id);
		}
		void begin(std::uint32_t id)
		{
			frame = id;
			++attempts;
			next = 0;
			active = true;
		}
		bool expects(std::uint32_t id, int point) const
		{
			return active && frame == id && next < sizeof(path) / sizeof(path[0]) && path[next] == point;
		}
		bool ready(std::uint32_t id) const
		{
			return active && frame == id && next == sizeof(path) / sizeof(path[0]);
		}
	};
}
