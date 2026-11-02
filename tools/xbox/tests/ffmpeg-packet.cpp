// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone build command is documented in docs/xbox/xbox_series_s_video.md.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "video_core/host1x/ffmpeg_packet.h"

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    for (const std::size_t size : {1U, 3U, 63U, 64U, 65U, 4096U, 65537U}) {
        std::vector<std::uint8_t> source(size, 0xa5);
        AVPacket* reference = av_packet_alloc();
        Check(reference != nullptr, "allocate reference");
        {
            FFmpeg::Packet packet(source);
            const auto* raw = packet.GetPacket();
            Check(raw && raw->buf && raw->size == static_cast<int>(size), "owned payload");
            Check(raw->data != source.data(), "independent allocation");
            Check(std::equal(source.begin(), source.end(), raw->data), "payload exact");
            Check(std::all_of(raw->data + size, raw->data + size + AV_INPUT_BUFFER_PADDING_SIZE,
                              [](auto byte) { return byte == 0; }),
                  "zero padding");
            Check(av_packet_ref(reference, raw) == 0, "FFmpeg retains packet");
            source.clear();
            source.shrink_to_fit();
        }
        Check(reference->data[0] == 0xa5 && reference->data[size - 1] == 0xa5,
              "retained payload survives source and wrapper destruction");
        av_packet_free(&reference);
    }
    Check(FFmpeg::Packet({}).GetPacket() == nullptr, "empty input rejected");
    av_max_alloc(1);
    const std::array<std::uint8_t, 8> bytes{};
    Check(FFmpeg::Packet(bytes).GetPacket() == nullptr, "allocation failure rejected");
    av_max_alloc(512);
    const std::vector<std::uint8_t> large(65537, 0xa5);
    Check(FFmpeg::Packet(large).GetPacket() == nullptr, "payload allocation failure rejected");
    av_max_alloc(std::numeric_limits<std::size_t>::max());

    // A real codec reads packets made by the production wrapper.
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_RAWVIDEO);
    Check(codec != nullptr, "rawvideo decoder available");
    AVCodecContext* context = avcodec_alloc_context3(codec);
    Check(context != nullptr, "allocate decoder");
    context->width = 16;
    context->height = 16;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    Check(avcodec_open2(context, codec, nullptr) == 0, "open decoder");
    AVFrame* frame = av_frame_alloc();
    Check(frame != nullptr, "allocate output");
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        std::vector<std::uint8_t> pixels(16 * 16 * 3 / 2, static_cast<std::uint8_t>(iteration));
        {
            FFmpeg::Packet packet(pixels);
            Check(avcodec_send_packet(context, packet.GetPacket()) == 0, "submit packet");
        }
        pixels.clear();
        pixels.shrink_to_fit();
        Check(avcodec_receive_frame(context, frame) == 0, "receive decoded frame");
        for (int plane = 0; plane < 3; ++plane) {
            const int extent = plane == 0 ? 16 : 8;
            for (int y = 0; y < extent; ++y) {
                for (int x = 0; x < extent; ++x) {
                    Check(frame->data[plane][y * frame->linesize[plane] + x] == iteration,
                          "decoded pixels exact after source destruction");
                }
            }
        }
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
    avcodec_free_context(&context);
    std::puts("PASS: ownership, padding, allocation failure, 128 decoded frames");
}
