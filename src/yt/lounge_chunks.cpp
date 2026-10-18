// youtube-tv - Cast from the phone: splitting the lounge's chunked stream.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/lounge.hpp"

#include <cstdlib>

namespace yt
{

std::vector<std::string> take_lounge_chunks(std::string &buffer)
{
    std::vector<std::string> chunks;
    for (;;)
    {
        const std::size_t newline = buffer.find('\n');
        if (newline == std::string::npos)
            break;
        const long length = std::atol(buffer.substr(0, newline).c_str());
        if (length <= 0)
        {
            buffer.erase(0, newline + 1);
            continue;
        }
        // The length counts UTF-16 units (a phone's name can hold any
        // character): walk the UTF-8 bytes until that many are covered.
        std::size_t end = newline + 1;
        long units = 0;
        while (units < length && end < buffer.size())
        {
            const unsigned char lead = static_cast<unsigned char>(buffer[end]);
            const std::size_t bytes = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            if (end + bytes > buffer.size())
                break;
            end += bytes;
            units += bytes == 4 ? 2 : 1;
        }
        if (units < length)
            break; // the rest of the chunk is still on its way
        chunks.push_back(buffer.substr(newline + 1, end - newline - 1));
        buffer.erase(0, end);
    }
    return chunks;
}

} // namespace yt
