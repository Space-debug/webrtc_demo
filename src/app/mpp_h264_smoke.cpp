#include <cstdlib>
#include <cstring>
#include <iostream>

#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
#include "mpp_packet.h"
#include "mpp_task.h"
#include "rk_mpi.h"
#include "rk_mpi_cmd.h"
#include "rk_venc_cmd.h"
#include "rk_venc_cfg.h"

namespace {

int ReadEnvInt(const char* name, int fallback) {
    const char* e = std::getenv(name);
    if (!e || !e[0]) {
        return fallback;
    }
    return std::atoi(e);
}

constexpr int AlignUp(int v, int align) {
    return ((v + align - 1) / align) * align;
}

void FillNv12Pattern(uint8_t* base, int hor_stride, int ver_stride, int width, int height) {
    if (!base) {
        return;
    }
    for (int y = 0; y < height; ++y) {
        uint8_t* row = base + static_cast<size_t>(y) * static_cast<size_t>(hor_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = static_cast<uint8_t>((x + y) & 0xff);
        }
    }
    uint8_t* uv = base + static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride);
    for (int y = 0; y < height / 2; ++y) {
        uint8_t* row = uv + static_cast<size_t>(y) * static_cast<size_t>(hor_stride);
        for (int x = 0; x < width; x += 2) {
            row[x] = 128;
            row[x + 1] = 128;
        }
    }
}

void FillNv12PatternForFrame(uint8_t* base, int hor_stride, int ver_stride, int width, int height, int frame_index) {
    if (!base) {
        return;
    }
    for (int y = 0; y < height; ++y) {
        uint8_t* row = base + static_cast<size_t>(y) * static_cast<size_t>(hor_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = static_cast<uint8_t>((x + y + frame_index * 7) & 0xff);
        }
    }
    uint8_t* uv = base + static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride);
    for (int y = 0; y < height / 2; ++y) {
        uint8_t* row = uv + static_cast<size_t>(y) * static_cast<size_t>(hor_stride);
        for (int x = 0; x < width; x += 2) {
            row[x] = static_cast<uint8_t>(96 + (frame_index * 3) % 32);
            row[x + 1] = static_cast<uint8_t>(160 - (frame_index * 5) % 32);
        }
    }
}

}  // namespace

int main() {
    const int width = ReadEnvInt("MPP_SMOKE_WIDTH", 1280);
    const int height = ReadEnvInt("MPP_SMOKE_HEIGHT", 720);
    const int fps = ReadEnvInt("MPP_SMOKE_FPS", 10);
    const int hor_stride = width;
    const int ver_stride = height;
    const int bitrate = ReadEnvInt("MPP_SMOKE_BITRATE", 2'000'000);
    const int frame_count = ReadEnvInt("MPP_SMOKE_FRAMES", 4);
    const bool use_task = ReadEnvInt("MPP_SMOKE_USE_TASK", 0) == 1;
    const size_t nv12_size = static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride) * 3 / 2;

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        std::cerr << "mpp_create failed" << std::endl;
        return 1;
    }
    if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        std::cerr << "mpp_init failed" << std::endl;
        mpp_destroy(ctx);
        return 1;
    }

    RK_S64 input_timeout = 10;
    RK_S64 output_timeout = 1000;
    mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &input_timeout);
    mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &output_timeout);

    MppEncCfg cfg = nullptr;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK || !cfg) {
        std::cerr << "mpp_enc_cfg_init failed" << std::endl;
        mpp_destroy(ctx);
        return 1;
    }
    if (mpi->control(ctx, MPP_ENC_GET_CFG, cfg) != MPP_OK) {
        std::cerr << "MPP_ENC_GET_CFG failed" << std::endl;
        mpp_enc_cfg_deinit(cfg);
        mpp_destroy(ctx);
        return 1;
    }

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps * 2);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", -1);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 51);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:stream_type", 0);

    if (mpi->control(ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        std::cerr << "MPP_ENC_SET_CFG failed" << std::endl;
        mpp_enc_cfg_deinit(cfg);
        mpp_destroy(ctx);
        return 1;
    }
    mpp_enc_cfg_deinit(cfg);

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);

    MppPacket extra = nullptr;
    if (mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &extra) == MPP_OK && extra) {
        std::cout << "[mpp-smoke] extra_info_len=" << mpp_packet_get_length(extra) << std::endl;
        mpp_packet_deinit(&extra);
    } else {
        std::cout << "[mpp-smoke] extra_info unavailable" << std::endl;
    }

    const size_t mdinfo_size = static_cast<size_t>(AlignUp(hor_stride, 64) >> 6) *
                               static_cast<size_t>(AlignUp(ver_stride, 16) >> 4) * 16;
    MppBufferGroup grp = nullptr;
    MppBuffer frm_buf = nullptr;
    MppBuffer pkt_buf = nullptr;
    MppBuffer md_info = nullptr;
    if (mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM) != MPP_OK || !grp ||
        mpp_buffer_get(grp, &frm_buf, nv12_size) != MPP_OK || !frm_buf ||
        mpp_buffer_get(grp, &pkt_buf, nv12_size) != MPP_OK || !pkt_buf ||
        mpp_buffer_get(grp, &md_info, mdinfo_size) != MPP_OK || !md_info) {
        std::cerr << "buffer alloc failed" << std::endl;
        if (md_info) {
            mpp_buffer_put(md_info);
        }
        if (pkt_buf) {
            mpp_buffer_put(pkt_buf);
        }
        if (frm_buf) {
            mpp_buffer_put(frm_buf);
        }
        if (grp) {
            mpp_buffer_group_put(grp);
        }
        mpp_destroy(ctx);
        return 1;
    }
    for (int i = 0; i < frame_count; ++i) {
        FillNv12PatternForFrame(static_cast<uint8_t*>(mpp_buffer_get_ptr(frm_buf)), hor_stride, ver_stride, width, height,
                                i);

        MppFrame frame = nullptr;
        if (mpp_frame_init(&frame) != MPP_OK) {
            std::cerr << "mpp_frame_init failed at frame " << i << std::endl;
            break;
        }
        mpp_frame_set_width(frame, width);
        mpp_frame_set_height(frame, height);
        mpp_frame_set_hor_stride(frame, hor_stride);
        mpp_frame_set_ver_stride(frame, ver_stride);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame, frm_buf);
        mpp_frame_set_pts(frame, static_cast<RK_S64>(i));

        MppPacket packet = nullptr;
        MPP_RET ret = MPP_NOK;
        if (use_task) {
            MppTask input_task = nullptr;
            MppTask output_task = nullptr;
            ret = mpi->poll(ctx, MPP_PORT_INPUT, static_cast<MppPollType>(10));
            std::cout << "[mpp-smoke] frame#" << i << " poll_input_ret=" << ret << std::endl;
            if (ret == MPP_OK) {
                ret = mpi->dequeue(ctx, MPP_PORT_INPUT, &input_task);
                std::cout << "[mpp-smoke] frame#" << i << " dequeue_input_ret=" << ret
                          << " task=" << (input_task ? 1 : 0) << std::endl;
            }
            if (ret == MPP_OK && input_task) {
                mpp_task_meta_set_frame(input_task, KEY_INPUT_FRAME, frame);
                ret = mpi->enqueue(ctx, MPP_PORT_INPUT, input_task);
                std::cout << "[mpp-smoke] frame#" << i << " enqueue_input_ret=" << ret << std::endl;
            }
            mpp_frame_deinit(&frame);
            if (ret == MPP_OK) {
                ret = mpi->poll(ctx, MPP_PORT_OUTPUT, static_cast<MppPollType>(10));
                std::cout << "[mpp-smoke] frame#" << i << " poll_output_ret=" << ret << std::endl;
            }
            if (ret == MPP_OK) {
                ret = mpi->dequeue(ctx, MPP_PORT_OUTPUT, &output_task);
                std::cout << "[mpp-smoke] frame#" << i << " dequeue_output_ret=" << ret
                          << " task=" << (output_task ? 1 : 0) << std::endl;
            }
            if (ret == MPP_OK && output_task) {
                ret = mpp_task_meta_get_packet(output_task, KEY_OUTPUT_PACKET, &packet);
                std::cout << "[mpp-smoke] frame#" << i << " get_packet_ret=" << ret
                          << " packet=" << (packet ? 1 : 0) << std::endl;
                mpi->enqueue(ctx, MPP_PORT_OUTPUT, output_task);
            }
        } else {
            MppMeta meta = mpp_frame_get_meta(frame);
            if (meta) {
                mpp_packet_init_with_buffer(&packet, pkt_buf);
                if (packet) {
                    // Same requirement as mpi_enc_test: clear output packet length first.
                    mpp_packet_set_length(packet, 0);
                    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);
                }
                mpp_meta_set_buffer(meta, KEY_MOTION_INFO, md_info);
            }
            ret = mpi->encode_put_frame(ctx, frame);
            std::cout << "[mpp-smoke] frame#" << i << " encode_put_frame_ret=" << ret << std::endl;
            mpp_frame_deinit(&frame);
            ret = mpi->encode_get_packet(ctx, &packet);
            std::cout << "[mpp-smoke] frame#" << i << " encode_get_packet_ret=" << ret
                      << " packet=" << (packet ? 1 : 0) << std::endl;
        }

        if (packet) {
            std::cout << "[mpp-smoke] frame#" << i
                      << " packet_len=" << mpp_packet_get_length(packet)
                      << " is_eoi=" << mpp_packet_is_eoi(packet)
                      << " is_part=" << mpp_packet_is_partition(packet) << std::endl;
            mpp_packet_deinit(&packet);
        }
    }

    mpp_buffer_put(md_info);
    mpp_buffer_put(pkt_buf);
    mpp_buffer_put(frm_buf);
    mpp_buffer_group_put(grp);
    mpp_destroy(ctx);
    return 0;
}
