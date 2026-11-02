// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"

extern "C" {
#ifdef LIBVA_FOUND
// for querying VAAPI driver information
#include <libavutil/hwcontext_vaapi.h>
#endif

#include <libavutil/hwcontext.h>
}

namespace FFmpeg {

namespace {

constexpr AVPixelFormat PreferredGpuFormat = AV_PIX_FMT_NV12;
constexpr AVPixelFormat PreferredCpuFormat = AV_PIX_FMT_YUV420P;
constexpr std::array PreferredGpuDecoders = {
#if defined(_WIN32)
    AV_HWDEVICE_TYPE_CUDA,
    AV_HWDEVICE_TYPE_D3D11VA,
    AV_HWDEVICE_TYPE_DXVA2,
    AV_HWDEVICE_TYPE_D3D12VA,
#elif defined(__FreeBSD__)
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_VDPAU,
    AV_HWDEVICE_TYPE_DRM,
#elif defined(__APPLE__)
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(__ANDROID__)
    AV_HWDEVICE_TYPE_MEDIACODEC,
#elif defined(__unix__)
    AV_HWDEVICE_TYPE_CUDA,
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_VDPAU,
#endif
    AV_HWDEVICE_TYPE_VULKAN,
};

AVPixelFormat GetGpuFormat(AVCodecContext* codec_context, const AVPixelFormat* pix_fmts) {
    if (codec_context->hw_device_ctx) {
        const auto* device =
            reinterpret_cast<const AVHWDeviceContext*>(codec_context->hw_device_ctx->data);
        for (int i = 0;; i++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec_context->codec, i);
            if (!config) {
                break;
            }

            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                config->device_type == device->type) {
                for (const auto* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
                    if (*p == config->pix_fmt) {
                        return *p;
                    }
                }
            }
        }
    }

    // The fallback must be one of the formats offered for this stream. Returning
    // YUV420P unconditionally violates get_format's contract for other profiles.
    LOG_INFO(HW_GPU, "Could not find supported GPU pixel format, falling back to CPU decoder");
    for (const auto* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == PreferredCpuFormat) {
            return *p;
        }
    }
    for (const auto* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const auto* desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

std::string AVError(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_make_error_string(errbuf, sizeof(errbuf) - 1, errnum);
    return errbuf;
}

#if defined(__ANDROID__)
size_t FindNalStartCode(std::span<const u8> data, size_t i) {
    const size_t n = data.size();
    if (i + 3 < n && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
        return 4;
    }
    if (i + 2 < n && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        return 3;
    }
    return 0;
}

std::vector<u8> ExtractH264ParameterSetExtradata(std::span<const u8> packet) {
    std::vector<u8> extradata;
    const size_t size = packet.size();
    size_t i = 0;
    while (i < size) {
        const size_t sc = FindNalStartCode(packet, i);
        if (sc == 0) {
            ++i;
            continue;
        }
        const size_t nal_start = i + sc;
        if (nal_start >= size) {
            break;
        }
        const u8 nal_type = packet[nal_start] & 0x1F;

        size_t j = nal_start + 1;
        while (j < size && FindNalStartCode(packet, j) == 0) {
            ++j;
        }

        if (nal_type == 7 || nal_type == 8) {
            constexpr u8 start[4] = {0, 0, 0, 1};
            extradata.insert(extradata.end(), start, start + sizeof(start));
            extradata.insert(extradata.end(), packet.begin() + nal_start, packet.begin() + j);
        } else if (nal_type == 1 || nal_type == 5) {
            break;
        }
        i = j;
    }
    return extradata;
}
#endif

}

Frame::Frame() {
    m_frame = av_frame_alloc();
}

Frame::~Frame() {
    av_frame_free(&m_frame);
}

Decoder::Decoder(Tegra::Host1x::NvdecCommon::VideoCodec codec) {
    const AVCodecID av_codec = [&] {
        switch (codec) {
        case Tegra::Host1x::NvdecCommon::VideoCodec::H264:
            return AV_CODEC_ID_H264;
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP8:
            return AV_CODEC_ID_VP8;
        case Tegra::Host1x::NvdecCommon::VideoCodec::VP9:
            return AV_CODEC_ID_VP9;
        default:
            UNIMPLEMENTED_MSG("Unknown codec {}", codec);
            return AV_CODEC_ID_NONE;
        }
    }();

#if defined(__ANDROID__)
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        const char* mc_name = nullptr;
        switch (av_codec) {
        case AV_CODEC_ID_H264: mc_name = "h264_mediacodec"; break;
        // Disabled: causes crashes in Diablo II
        // case AV_CODEC_ID_VP8:  mc_name = "vp8_mediacodec";  break;
        case AV_CODEC_ID_VP9:  mc_name = "vp9_mediacodec";  break;
        default: break;
        }
        if (mc_name) {
            m_codec = avcodec_find_decoder_by_name(mc_name);
        }
    }
#endif
    if (!m_codec) {
        m_codec = avcodec_find_decoder(av_codec);
    }
}

bool Decoder::SupportsDecodingOnDevice(AVPixelFormat* out_pix_fmt, AVHWDeviceType type) const {
    if (!m_codec) {
        return false;
    }
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(m_codec, i);
        if (!config) {
            LOG_DEBUG(HW_GPU, "{} decoder does not support device type {}", m_codec->name, av_hwdevice_get_type_name(type));
            break;
        }

        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
            LOG_DEBUG(HW_GPU, "{} decoder supports {} GPU decoding", m_codec->name,
                      av_hwdevice_get_type_name(type));
            *out_pix_fmt = config->pix_fmt;
            return true;
        }
    }

    return false;
}

std::vector<AVHWDeviceType> HardwareContext::GetSupportedDeviceTypes() {
    std::vector<AVHWDeviceType> types;
    AVHWDeviceType current_device_type = AV_HWDEVICE_TYPE_NONE;

    while (true) {
        current_device_type = av_hwdevice_iterate_types(current_device_type);
        if (current_device_type == AV_HWDEVICE_TYPE_NONE) {
            return types;
        }

        types.push_back(current_device_type);
    }
}

HardwareContext::~HardwareContext() {
    av_buffer_unref(&m_gpu_decoder);
}

bool HardwareContext::InitializeForDecoder(DecoderContext& decoder_context, const Decoder& decoder) {
    const auto supported_types = GetSupportedDeviceTypes();
    for (const auto type : PreferredGpuDecoders) {
        AVPixelFormat hw_pix_fmt;

        if (std::ranges::find(supported_types, type) == supported_types.end()) {
            LOG_DEBUG(HW_GPU, "{} explicitly unsupported", av_hwdevice_get_type_name(type));
            continue;
        }

        if (!decoder.SupportsDecodingOnDevice(&hw_pix_fmt, type)) {
            continue;
        }

        if (this->InitializeWithType(type)) {
            decoder_context.InitializeHardwareDecoder(*this, hw_pix_fmt);
            LOG_INFO(HW_GPU, "Using {} GPU decoder", av_hwdevice_get_type_name(type));
            return true;
        }
    }

    return false;
}

bool HardwareContext::InitializeWithType(AVHWDeviceType type) {
    av_buffer_unref(&m_gpu_decoder);

    if (const int ret = av_hwdevice_ctx_create(&m_gpu_decoder, type, nullptr, nullptr, 0); ret < 0) {
        LOG_DEBUG(HW_GPU, "av_hwdevice_ctx_create({}) failed: {}", av_hwdevice_get_type_name(type), AVError(ret));
        return false;
    }

#ifdef LIBVA_FOUND
    if (type == AV_HWDEVICE_TYPE_VAAPI) {
        // We need to determine if this is an impersonated VAAPI driver.
        auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(m_gpu_decoder->data);
        auto* vactx = static_cast<AVVAAPIDeviceContext*>(hwctx->hwctx);
        const char* vendor_name = vaQueryVendorString(vactx->display);
        if (strstr(vendor_name, "VDPAU backend")) {
            // VDPAU impersonated VAAPI impls are super buggy, we need to skip them.
            LOG_DEBUG(HW_GPU, "Skipping VDPAU impersonated VAAPI driver");
            return false;
        } else {
            // According to some user testing, certain VAAPI drivers (Intel?) could be buggy.
            // Log the driver name just in case.
            LOG_DEBUG(HW_GPU, "Using VAAPI driver: {}", vendor_name);
        }
    }
#endif

    return true;
}

DecoderContext::DecoderContext(const Decoder& decoder) : m_decoder{decoder} {
    if (!m_decoder.GetCodec()) {
        return;
    }
    m_codec_context = avcodec_alloc_context3(m_decoder.GetCodec());
    if (!m_codec_context) {
        return;
    }
    av_opt_set(m_codec_context->priv_data, "tune", "zerolatency", 0);
    m_codec_context->thread_count = 0;
    m_codec_context->thread_type &= ~FF_THREAD_FRAME;
#if defined(__ANDROID__)
    m_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
}

DecoderContext::~DecoderContext() {
    avcodec_free_context(&m_codec_context);
}

void DecoderContext::InitializeHardwareDecoder(const HardwareContext& context, AVPixelFormat hw_pix_fmt) {
    m_codec_context->hw_device_ctx = av_buffer_ref(context.GetBufferRef());
    m_codec_context->get_format = GetGpuFormat;
    m_codec_context->pix_fmt = hw_pix_fmt;
}

bool DecoderContext::OpenContext(const Decoder& decoder, std::span<const u8> extradata) {
    if (!m_codec_context || !decoder.GetCodec()) {
        LOG_ERROR(HW_GPU, "Video decoder unavailable or context allocation failed");
        return false;
    }
    if (extradata.size() > std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE) {
        LOG_ERROR(HW_GPU, "Video extradata is too large");
        return false;
    }
    if (!extradata.empty()) {
        av_freep(&m_codec_context->extradata);
        m_codec_context->extradata = static_cast<u8*>(
            av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_codec_context->extradata) {
            LOG_ERROR(HW_GPU, "Failed to allocate extradata");
            return false;
        }
        std::memcpy(m_codec_context->extradata, extradata.data(), extradata.size());
        m_codec_context->extradata_size = static_cast<int>(extradata.size());
    }

    if (const int ret = avcodec_open2(m_codec_context, decoder.GetCodec(), nullptr); ret < 0) {
        LOG_ERROR(HW_GPU, "avcodec_open2 error: {}", AVError(ret));
        return false;
    }

    LOG_INFO(HW_GPU, "Using decoder {}", decoder.GetCodec()->name);

    return true;
}

bool DecoderContext::SendPacket(const Packet& packet) {
    if (!m_codec_context || !packet.GetPacket()) {
        LOG_ERROR(HW_GPU, "Rejected empty or unallocated video packet");
        return false;
    }
    int ret = avcodec_send_packet(m_codec_context, packet.GetPacket());
    if (ret == AVERROR(EAGAIN)) {
        // EAGAIN means the packet was NOT consumed. Preserve decoded output,
        // then retry this same packet once, as required by the send/receive API.
        while (auto frame = ReceiveFrameImpl()) {
            m_ready_frames.push(std::move(frame));
        }
        ret = avcodec_send_packet(m_codec_context, packet.GetPacket());
    }
    if (ret < 0) {
        LOG_ERROR(HW_GPU, "avcodec_send_packet error: {}", AVError(ret));
        return false;
    }

    return true;
}

std::shared_ptr<Frame> DecoderContext::ReceiveFrame() {
    if (!m_ready_frames.empty()) {
        auto frame = std::move(m_ready_frames.front());
        m_ready_frames.pop();
        return frame;
    }
    return ReceiveFrameImpl();
}

std::shared_ptr<Frame> DecoderContext::ReceiveFrameImpl() {
    if (!m_codec_context) {
        return {};
    }
    auto ReceiveImpl = [&](AVFrame* frame) -> int {
        const int ret = avcodec_receive_frame(m_codec_context, frame);
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            LOG_ERROR(HW_GPU, "avcodec_receive_frame error: {}", AVError(ret));
        }
        return ret;
    };

    std::shared_ptr<Frame> intermediate_frame = std::make_shared<Frame>();
    if (!intermediate_frame->GetFrame()) {
        LOG_ERROR(HW_GPU, "Failed to allocate decoded video frame");
        return {};
    }
    if (ReceiveImpl(intermediate_frame->GetFrame()) < 0) {
        return {};
    }

    // A decoder with a hardware context can still output software frames after
    // format negotiation falls back. Test the frame, not just the device.
    if (intermediate_frame->IsHardwareDecoded()) {
        auto final_frame = std::make_shared<Frame>();
        if (!final_frame->GetFrame()) {
            LOG_ERROR(HW_GPU, "Failed to allocate transferred video frame");
            return {};
        }
        final_frame->SetFormat(PreferredGpuFormat);
        if (const int ret = av_hwframe_transfer_data(final_frame->GetFrame(),
                                                    intermediate_frame->GetFrame(), 0);
            ret < 0) {
            LOG_ERROR(HW_GPU, "av_hwframe_transfer_data error: {}", AVError(ret));
            return {};
        }
        return final_frame;
    }
    return intermediate_frame;
}

void DecodeApi::Reset() {
    m_hardware_context.reset();
    m_decoder_context.reset();
    m_decoder.reset();
    m_opened = false;
    m_defer_android_mediacodec_open = false;
    m_needs_h264_extradata = false;
    m_next_pts = 0;
    while (!m_pending_offsets.empty()) {
        m_pending_offsets.pop();
    }
}

bool DecodeApi::Initialize(Tegra::Host1x::NvdecCommon::VideoCodec codec) {
    this->Reset();
    m_decoder.emplace(codec);
    m_decoder_context.emplace(*m_decoder);
    if (!m_decoder->GetCodec() || !m_decoder_context->GetCodecContext()) {
        LOG_ERROR(HW_GPU, "Failed to initialize video decoder for codec {}", codec);
        this->Reset();
        return false;
    }

    bool is_mediacodec = false;
#if defined(__ANDROID__)
    const std::string_view decoder_name = m_decoder->GetCodec() ? m_decoder->GetCodec()->name : "";
    is_mediacodec = decoder_name == "h264_mediacodec" ||
                    decoder_name == "vp8_mediacodec" ||
                    decoder_name == "vp9_mediacodec";
#endif

    // Enable GPU decoding if requested.
    if (!is_mediacodec &&
        Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        m_hardware_context.emplace();
        m_hardware_context->InitializeForDecoder(*m_decoder_context, *m_decoder);
    }

#if defined(__ANDROID__)
    m_defer_android_mediacodec_open = is_mediacodec;
    m_needs_h264_extradata = decoder_name == "h264_mediacodec";
    if (m_defer_android_mediacodec_open) {
        return true;
    }
#endif

    // Open the decoder context.
    if (!m_decoder_context->OpenContext(*m_decoder)) {
        this->Reset();
        return false;
    }
    m_opened = true;

    return true;
}

bool DecodeApi::SendPacket(std::span<const u8> packet_data, const FrameOffsets& offsets,
                           std::optional<FrameDimensions> dimensions) {
    if (!m_decoder_context) {
        return false;
    }
    if (!m_opened) {
        std::vector<u8> extradata;
#if defined(__ANDROID__)
        if (m_defer_android_mediacodec_open) {
            if (!dimensions) {
                return true;
            }

            auto* ctx = m_decoder_context->GetCodecContext();
            ctx->width = dimensions->width;
            ctx->height = dimensions->height;
            ctx->coded_width = dimensions->width;
            ctx->coded_height = dimensions->height;
        }

        if (m_needs_h264_extradata) {
            extradata = ExtractH264ParameterSetExtradata(packet_data);
            if (extradata.empty()) {
                return true;
            }
        }
#endif
        if (!m_decoder_context->OpenContext(*m_decoder, extradata)) {
            this->Reset();
            return false;
        }
        m_opened = true;
    }
    FFmpeg::Packet packet(packet_data);
    if (!packet.GetPacket()) {
        LOG_ERROR(HW_GPU, "Failed to prepare video packet ({} bytes)", packet_data.size());
        return false;
    }
    packet.GetPacket()->pts = m_next_pts;
    packet.GetPacket()->dts = m_next_pts;
    if (!m_decoder_context->SendPacket(packet)) {
        return false;
    }
    if (!offsets.hidden) {
        m_pending_offsets.push(offsets);
    }
    ++m_next_pts;
    return true;
}

std::optional<DecodeApi::DecodedFrame> DecodeApi::ReceiveFrame() {
    if (!m_decoder_context) {
        return std::nullopt;
    }
    auto frame = m_decoder_context->ReceiveFrame();
    if (!frame) {
        return std::nullopt;
    }
    FrameOffsets offsets{};
    if (!m_pending_offsets.empty()) {
        offsets = m_pending_offsets.front();
        m_pending_offsets.pop();
    }
    return DecodedFrame{std::move(frame), offsets};
}

}
