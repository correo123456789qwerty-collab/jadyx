// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone integration gate: compile the production implementation, including
// its format callback. Unused emulator/settings sections are discarded at link.
#include "video_core/host1x/ffmpeg.cpp"

#include <cstdio>
#include <cstdlib>
#include <tuple>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

using Picture = std::tuple<int64_t, int, int, int, std::vector<uint8_t>>;

Picture Pixels(const AVFrame* frame) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const int size = av_image_get_buffer_size(format, frame->width, frame->height, 1);
    Check(size > 0, "valid image size");
    std::vector<uint8_t> pixels(size);
    Check(av_image_copy_to_buffer(pixels.data(), size, frame->data, frame->linesize, format,
                                  frame->width, frame->height, 1) == size,
          "copy visible pixels");
    return {frame->pts, frame->width, frame->height, frame->format, std::move(pixels)};
}

void CheckFormats() {
    FFmpeg::Decoder decoder(Tegra::Host1x::NvdecCommon::VideoCodec::H264);
    FFmpeg::DecoderContext context(decoder);
    auto* raw = context.GetCodecContext();
    Check(raw != nullptr, "allocate format context");
    const AVPixelFormat cpu[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    const AVPixelFormat ten_bit[] = {AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NONE};
    const AVPixelFormat hardware_only[] = {AV_PIX_FMT_CUDA, AV_PIX_FMT_NONE};
    const AVPixelFormat empty[] = {AV_PIX_FMT_NONE};
    Check(FFmpeg::GetGpuFormat(raw, cpu) == AV_PIX_FMT_YUV420P, "offered CPU format");
    Check(FFmpeg::GetGpuFormat(raw, ten_bit) == AV_PIX_FMT_YUV420P10LE,
          "fallback belongs to offered formats");
    Check(FFmpeg::GetGpuFormat(raw, hardware_only) == AV_PIX_FMT_NONE,
          "no hardware format without matching device");
    Check(FFmpeg::GetGpuFormat(raw, empty) == AV_PIX_FMT_NONE, "empty format list");

    // No GPU is needed: only inspect configuration metadata for each backend.
    for (int i = 0; const auto* config = avcodec_get_hw_config(decoder.GetCodec(), i); ++i) {
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            continue;
        }
        raw->hw_device_ctx = av_buffer_allocz(sizeof(AVHWDeviceContext));
        Check(raw->hw_device_ctx != nullptr, "allocate device metadata");
        auto* device = reinterpret_cast<AVHWDeviceContext*>(raw->hw_device_ctx->data);
        device->type = config->device_type;
        const AVPixelFormat offered[] = {config->pix_fmt, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
        Check(FFmpeg::GetGpuFormat(raw, offered) == config->pix_fmt, "matching device format");
        Check(FFmpeg::GetGpuFormat(raw, ten_bit) == AV_PIX_FMT_YUV420P10LE,
              "CPU fallback with hardware device retained");
        Check(raw->hw_device_ctx != nullptr, "callback preserves device ownership");
        av_buffer_unref(&raw->hw_device_ctx);
    }
    Check(!context.SendPacket(FFmpeg::Packet({})), "null packet is not a flush");
    av_max_alloc(1);
    Check(!context.ReceiveFrame(), "frame allocation failure is contained");
    {
        FFmpeg::DecoderContext failed(decoder);
        Check(!failed.GetCodecContext(), "simulate context allocation failure");
        Check(!failed.OpenContext(decoder), "reject unavailable context");
        Check(!failed.ReceiveFrame(), "receive with unavailable context");
    }
    av_max_alloc(std::numeric_limits<std::size_t>::max());
    FFmpeg::DecodeApi uninitialized;
    Check(!uninitialized.UsingDecodeOrder(), "uninitialized decode order");
    Check(!uninitialized.ReceiveFrame(), "uninitialized receive");
    Check(!uninitialized.SendPacket({}, {}), "uninitialized send");
}

void DecodeFile(const char* path, bool software_fallback) {
    AVFormatContext* input = nullptr;
    Check(avformat_open_input(&input, path, nullptr, nullptr) == 0, "open synthetic fixture");
    Check(avformat_find_stream_info(input, nullptr) >= 0, "read stream metadata");
    const int stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    Check(stream >= 0, "find video stream");
    const auto* params = input->streams[stream]->codecpar;
    const auto codec = [&] {
        using Codec = Tegra::Host1x::NvdecCommon::VideoCodec;
        switch (params->codec_id) {
        case AV_CODEC_ID_H264:
            return Codec::H264;
        case AV_CODEC_ID_VP8:
            return Codec::VP8;
        case AV_CODEC_ID_VP9:
            return Codec::VP9;
        default:
            Check(false, "supported fixture codec");
            return Codec::H264;
        }
    }();
    FFmpeg::Decoder decoder(codec);
    FFmpeg::DecoderContext actual(decoder);
    if (software_fallback) {
        // Exercise software frames while a device reference remains attached.
        // This metadata-only device deliberately has no supported accelerator;
        // get_format must pick CPU and ReceiveFrame must skip GPU readback.
        auto* context = actual.GetCodecContext();
        Check(context != nullptr, "allocate fallback context");
        context->hw_device_ctx = av_buffer_allocz(sizeof(AVHWDeviceContext));
        Check(context->hw_device_ctx != nullptr, "allocate fallback device metadata");
        context->get_format = FFmpeg::GetGpuFormat;
    }
    Check(actual.OpenContext(decoder,
                             {params->extradata, static_cast<std::size_t>(params->extradata_size)}),
          "open production decoder");
    AVCodecContext* reference = avcodec_alloc_context3(decoder.GetCodec());
    Check(reference != nullptr, "allocate reference decoder");
    Check(avcodec_parameters_to_context(reference, params) == 0, "copy reference parameters");
    reference->thread_count = 0;
    reference->thread_type &= ~FF_THREAD_FRAME;
    Check(avcodec_open2(reference, decoder.GetCodec(), nullptr) == 0, "open reference decoder");
    AVFrame* frame = av_frame_alloc();
    AVPacket* demuxed = av_packet_alloc();
    Check(frame && demuxed, "allocate reference frame and packet");
    std::vector<Picture> expected;
    std::vector<Picture> decoded;
    unsigned packets = 0;

    auto receive_reference = [&] {
        int ret;
        while ((ret = avcodec_receive_frame(reference, frame)) == 0) {
            expected.push_back(Pixels(frame));
            av_frame_unref(frame);
        }
        Check(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF, "reference output status");
    };
    auto receive_actual = [&] {
        while (auto output = actual.ReceiveFrame()) {
            decoded.push_back(Pixels(output->GetFrame()));
        }
    };

    while (av_read_frame(input, demuxed) >= 0) {
        if (demuxed->stream_index == stream) {
            Check(avcodec_send_packet(reference, demuxed) == 0, "reference accepts packet");
            receive_reference();
            {
                FFmpeg::Packet packet({demuxed->data, static_cast<std::size_t>(demuxed->size)});
                Check(packet.GetPacket() != nullptr, "copy compressed fixture packet");
                packet.GetPacket()->pts = demuxed->pts;
                packet.GetPacket()->dts = demuxed->dts;
                Check(actual.SendPacket(packet), "production accepts packet under backpressure");
            }
            ++packets;
            // Batches of 11 deliberately fill FFmpeg's output buffer. Input
            // must survive EAGAIN; every decoded frame is checked byte for byte.
            if (packets % 11 == 0) {
                receive_actual();
            }
        }
        av_packet_unref(demuxed);
    }
    receive_actual();
    Check(avcodec_send_packet(reference, nullptr) == 0, "flush reference at end of fixture");
    Check(avcodec_send_packet(actual.GetCodecContext(), nullptr) == 0, "flush production decoder");
    receive_reference();
    receive_actual();
    Check(expected.size() >= 30, "enough frames exercised");
    Check(decoded == expected, "all timestamps, formats, dimensions and pixels match reference");
    if (software_fallback) {
        Check(actual.GetCodecContext()->hw_device_ctx != nullptr,
              "software output preserves device reference");
    }

    const std::array<u8, 8> invalid{};
    Check(!actual.SendPacket(FFmpeg::Packet(invalid)), "EOF rejects new input");
    std::printf("PASS: %s%s, %u packets, %zu exact frames, batched EAGAIN and EOF\n",
                decoder.GetCodec()->name, software_fallback ? " (software fallback)" : "", packets,
                decoded.size());
    av_packet_free(&demuxed);
    av_frame_free(&frame);
    avcodec_free_context(&reference);
    avformat_close_input(&input);
}

} // namespace

// Only the logger and assertion policy are replaced; decoding uses libavcodec.
void Common::Log::FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*,
                                    fmt::string_view, const fmt::format_args&) {}
void AssertFailSoftImpl() {
    Check(false, "unexpected emulator assertion");
}
[[noreturn]] void AssertFatalImpl() {
    std::abort();
}

int main(int argc, char** argv) {
    Check(argc > 1, "supply synthetic video fixtures");
    CheckFormats();
    for (int i = 1; i < argc; ++i) {
        DecodeFile(argv[i], false);
        DecodeFile(argv[i], true);
    }
    std::puts("PASS: format negotiation, allocation failure and uninitialized API");
}
