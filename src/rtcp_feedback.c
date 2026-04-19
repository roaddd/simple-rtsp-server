#include "session.h"
#include "rtcp_feedback.h"

/* 大端读取工具：RTCP/RTP 头字段均为网络字节序 */
static uint16_t readBe16(const uint8_t *p){
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t readBe32(const uint8_t *p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int32_t readSigned24(const uint8_t *p){
    int32_t value = ((int32_t)p[0] << 16) | ((int32_t)p[1] << 8) | (int32_t)p[2];
    if(value & 0x00800000){
        value |= (int32_t)0xFF000000;
    }
    return value;
}
static uint32_t ntpMiddle32(uint64_t ntp64){
    return (uint32_t)((ntp64 >> 16) & 0xFFFFFFFFU);
}
static uint32_t ntpDiffToMs(uint32_t ntp_diff_16_16){
    uint32_t sec = ntp_diff_16_16 >> 16;
    uint32_t frac = ntp_diff_16_16 & 0xFFFFU;
    return sec * 1000U + (frac * 1000U) / 65536U;
}
/* 按 TCP interleaved 通道号映射到视频/音频 RTCP 统计上下文 */
static struct RtcpReceiverContext *selectRtcpRxContextByChannel(struct clientinfo_st *clientinfo, uint8_t channel){
    if(clientinfo == NULL){
        return NULL;
    }
    if(clientinfo->sig_1 >= 0 && channel == (uint8_t)clientinfo->sig_1){
        return &clientinfo->rtcp_rx_video;
    }
    if(clientinfo->sig_3 >= 0 && channel == (uint8_t)clientinfo->sig_3){
        return &clientinfo->rtcp_rx_audio;
    }
    return NULL;
}
/* UDP 场景按媒体类型映射统计上下文 */
static struct RtcpReceiverContext *selectRtcpRxContextByMedia(struct clientinfo_st *clientinfo, int is_audio){
    if(clientinfo == NULL){
        return NULL;
    }
    return is_audio ? &clientinfo->rtcp_rx_audio : &clientinfo->rtcp_rx_video;
}
/* 通过报告块里的 SSRC 二次归类（防止通道映射不明确时统计串扰） */
static struct RtcpReceiverContext *selectRtcpRxContextBySsrc(struct clientinfo_st *clientinfo, uint32_t source_ssrc, struct RtcpReceiverContext *fallback){
    uint32_t video_ssrc = 0;
    uint32_t audio_ssrc = 0;
    if(clientinfo == NULL){
        return fallback;
    }
    if(clientinfo->rtp_packet != NULL){
        video_ssrc = clientinfo->rtp_packet->rtpHeader.ssrc;
    }
    if(clientinfo->rtp_packet_1 != NULL){
        audio_ssrc = clientinfo->rtp_packet_1->rtpHeader.ssrc;
    }
    if(video_ssrc != 0 && source_ssrc == video_ssrc){
        return &clientinfo->rtcp_rx_video;
    }
    if(audio_ssrc != 0 && source_ssrc == audio_ssrc){
        return &clientinfo->rtcp_rx_audio;
    }
    return fallback;
}
/**
 * @description: 解析 RR(report block) 并更新关键质量指标
 * 指标包括：丢包比例、累计丢包、最高序号、抖动、LSR/DLSR、RTT
 */
static void updateRtcpReceiverReport(struct clientinfo_st *clientinfo, struct RtcpReceiverContext *ctx, const uint8_t *packet, int packet_len, int report_count){
    int i;
    const uint8_t *p;
    if(clientinfo == NULL || ctx == NULL || packet == NULL){
        return;
    }
    if(packet_len < 8){
        return;
    }
    ctx->rr_count++;
    ctx->last_rx_time_ms = getTimeMs();
    p = packet + 8;
    for(i = 0; i < report_count && (p + 24) <= (packet + packet_len); i++){
        struct RtcpReceiverContext *target_ctx = ctx;
        uint32_t source_ssrc = readBe32(p);
        uint32_t arrival_ntp_middle = ntpMiddle32(getNtpTimestamp64());
        uint32_t lsr;
        uint32_t dlsr;

        target_ctx = selectRtcpRxContextBySsrc(clientinfo, source_ssrc, target_ctx);
        target_ctx->last_reportee_ssrc = source_ssrc;
        target_ctx->fraction_lost = p[4];
        target_ctx->cumulative_lost = readSigned24(p + 5);
        target_ctx->highest_seq = readBe32(p + 8);
        target_ctx->jitter = readBe32(p + 12);
        lsr = readBe32(p + 16);
        dlsr = readBe32(p + 20);
        target_ctx->lsr = lsr;
        target_ctx->dlsr = dlsr;
        // RTT 估算: RTT = A - LSR - DLSR (单位 16.16 NTP)
        if(lsr != 0 && dlsr != 0){
            uint64_t lsr_dlsr = (uint64_t)lsr + (uint64_t)dlsr;
            if((uint64_t)arrival_ntp_middle > lsr_dlsr){
                uint32_t rtt_ntp = arrival_ntp_middle - (uint32_t)lsr_dlsr;
                target_ctx->rtt_ms = ntpDiffToMs(rtt_ntp);
            }
        }
        target_ctx->last_rx_time_ms = ctx->last_rx_time_ms;
        p += 24;
    }
}
/**
 * @description: 解析一个 RTCP compound packet，并按 packet type 统计
 * 支持: SR(200)、RR(201)、SDES(202)、BYE(203)
 */
static void rtcpHandlePacket(struct clientinfo_st *clientinfo, struct RtcpReceiverContext *ctx, const uint8_t *payload, int payload_len){
    const uint8_t *p = payload;
    int remain = payload_len;

    if(clientinfo == NULL || ctx == NULL || payload == NULL || payload_len <= 0){
        return;
    }
    ctx->packet_count++;
    ctx->last_rx_time_ms = getTimeMs();
    while(remain >= 4){
        int version = (p[0] >> 6) & 0x03;
        int report_count = p[0] & 0x1F;
        int packet_type = p[1];
        // length 字段单位是 32-bit words - 1
        int words = readBe16(p + 2) + 1;
        int packet_len = words * 4;
        if(version != RTP_VESION || packet_len < 4 || packet_len > remain){
            break;
        }
        switch(packet_type){
            case 200:
                ctx->sr_count++;
                break;
            case 201:
                updateRtcpReceiverReport(clientinfo, ctx, p, packet_len, report_count);
                break;
            case 202:
                ctx->sdes_count++;
                break;
            case 203:
                ctx->bye_count++;
                break;
            default:
                break;
        }
        p += packet_len;
        remain -= packet_len;
    }
}
/* RTP-over-TCP 入口 */
void rtcpHandleInterleaved(struct clientinfo_st *clientinfo, uint8_t channel, const uint8_t *payload, int payload_len){
    struct RtcpReceiverContext *ctx = selectRtcpRxContextByChannel(clientinfo, channel);
    rtcpHandlePacket(clientinfo, ctx, payload, payload_len);
}
/* RTP-over-UDP 入口 */
void rtcpHandleUdp(struct clientinfo_st *clientinfo, int is_audio, const uint8_t *payload, int payload_len){
    struct RtcpReceiverContext *ctx = selectRtcpRxContextByMedia(clientinfo, is_audio);
    rtcpHandlePacket(clientinfo, ctx, payload, payload_len);
}
