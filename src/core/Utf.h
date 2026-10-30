#pragma once

#include <cstdint>
#include <string>

// UTF-8 to UTF-16, with characters beyond U+FFFF (emoji such as the Daily Challenge's green and yellow squares) as
// surrogate pairs. ImGui's clipboard drops those (its wide characters are 16-bit), so the share text is copied with
// this. Invalid bytes become U+FFFD. No SDK, no Windows calls.
namespace utf
{
	inline std::u16string utf8ToUtf16(const std::string& in)
	{
		std::u16string out;
		out.reserve(in.size());
		size_t i = 0;
		while (i < in.size())
		{
			const unsigned char c = static_cast<unsigned char>(in[i]);
			uint32_t cp = 0xFFFD;
			size_t len = 1;
			if (c < 0x80)
				cp = c;
			else if ((c >> 5) == 0x6)
				len = 2, cp = c & 0x1F;
			else if ((c >> 4) == 0xE)
				len = 3, cp = c & 0x0F;
			else if ((c >> 3) == 0x1E)
				len = 4, cp = c & 0x07;
			else
				len = 0; // a stray continuation byte
			if (len > 1)
			{
				bool ok = i + len <= in.size();
				for (size_t k = 1; ok && k < len; ++k)
				{
					const unsigned char cc = static_cast<unsigned char>(in[i + k]);
					ok = (cc >> 6) == 0x2;
					cp = (cp << 6) | (cc & 0x3F);
				}
				if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
				{
					cp = 0xFFFD;
					len = 1;
				}
			}
			else if (len == 0)
			{
				cp = 0xFFFD;
				len = 1;
			}
			if (cp >= 0x10000)
			{
				cp -= 0x10000;
				out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
				out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
			}
			else
				out.push_back(static_cast<char16_t>(cp));
			i += len;
		}
		return out;
	}
}
