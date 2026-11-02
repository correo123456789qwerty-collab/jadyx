// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace FFmpeg {

// Own the bitstream, including the zero padding required by FFmpeg's readers.
// A null packet denotes invalid input or allocation failure, never a flush request.
class Packet {
public:
    explicit Packet(std::span<const std::uint8_t> data) {
        if (data.empty() ||
            data.size() > std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE) {
            return;
        }
        m_packet = av_packet_alloc();
        if (!m_packet) {
            return;
        }
        if (av_new_packet(m_packet, static_cast<int>(data.size())) < 0) {
            av_packet_free(&m_packet);
            return;
        }
        std::memcpy(m_packet->data, data.data(), data.size());
    }

    ~Packet() {
        av_packet_free(&m_packet);
    }

    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;
    Packet(Packet&&) = delete;
    Packet& operator=(Packet&&) = delete;

    AVPacket* GetPacket() const {
        return m_packet;
    }

private:
    AVPacket* m_packet{};
};

} // namespace FFmpeg
