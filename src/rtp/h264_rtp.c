#include "rtp.h"

/*
 * H264 over RTP 关键协议点：
 *
 * 1. RTP 固定头字段（12 字节，见 common.h::RtpHeader）
 *    - version: RTP 版本，固定为 2。
 *    - payloadType: 负载类型。H264 常用动态负载类型 96，这里由 session 初始化。
 *    - seq: RTP 序列号，每发一个 RTP 包递增 1，用于丢包检测和乱序重排。
 *    - timestamp: RTP 时间戳。H264 使用 90000Hz 时钟；同一帧拆出的所有 RTP 包必须相同。
 *    - marker: 对 H264 来说表示 access unit 边界，通常只在一帧最后一个 RTP 包置 1。
 *    - ssrc: 同一媒体流的同步源标识，客户端用它区分不同 RTP 流。
 *
 * 2. 单 NALU RTP payload
 *    当一个 NALU 可以放入一个 RTP 包时，payload 直接是：
 *      +---------------+-------------------+
 *      | H264 NAL hdr  | NAL payload ...   |
 *      +---------------+-------------------+
 *    NAL hdr 字段：
 *      F(1bit) | NRI(2bit) | Type(5bit)
 *
 * 3. FU-A 分片 RTP payload（RFC 6184）
 *    当一个 NALU 超过单包负载上限时，拆成多个 FU-A：
 *      +---------------+---------------+-------------------+
 *      | FU indicator  | FU header     | FU payload ...    |
 *      +---------------+---------------+-------------------+
 *    FU indicator:
 *      F = 原 NALU F
 *      NRI = 原 NALU NRI
 *      Type = 28，表示 FU-A
 *    FU header:
 *      S = 1 表示该 NALU 第一个分片
 *      E = 1 表示该 NALU 最后一个分片
 *      R = 0 保留位
 *      Type = 原 NALU Type
 *
 * 4. 输入格式
 *    rtpSendH264Frame() 接收一帧 Annex-B 数据，内部拆出多个 NALU。
 *    起始码 00 00 01 / 00 00 00 01 只用于本地分割，不会写入 RTP payload。
 */

/* 返回 Annex-B 起始码长度：3 字节(00 00 01)或 4 字节。 */
static int h264_start_code_len(const uint8_t *data, uint32_t len)
{
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return 4;
    }
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return 3;
    }
    return 0;
}

/* 从 offset 开始查找下一个 Annex-B 起始码位置。 */
static int h264_find_start_code(const uint8_t *data, uint32_t len, uint32_t offset, uint32_t *pos, int *code_len)
{
    uint32_t i;
    if (!data || !pos || !code_len || offset >= len) {
        return -1;
    }
    for (i = offset; i + 3 <= len; ++i) {
        int cur = h264_start_code_len(data + i, len - i);
        if (cur > 0) {
            *pos = i;
            *code_len = cur;
            return 0;
        }
    }
    return -1;
}

static int h264_send_rtp_packet(socket_t sd,
                                struct rtp_tcp_header *tcp_header,
                                struct RtpPacket *rtp_packet,
                                const uint8_t *payload,
                                uint32_t payload_size,
                                uint32_t rtp_timestamp,
                                int marker,
                                int sig_0,
                                char *client_ip,
                                int client_rtp_port)
{
    int ret;

    /*
     * RTP 头字段在内存中保持主机字节序；真正发送前再转网络字节序。
     * marker 按 access unit 边界设置，不是每个 NALU 都置 1。
     */
    rtp_packet->rtpHeader.marker = marker ? 1 : 0;
    rtp_packet->rtpHeader.timestamp = rtp_timestamp;
    memcpy(rtp_packet->payload, payload, payload_size);

    if (tcp_header != NULL && sig_0 != -1) {
        /*
         * RTP over RTSP/TCP 使用 interleaved frame：
         *   '$'(1B) | channel(1B) | length(2B) | RTP packet
         * channel 对应 SETUP 里的 interleaved=x-y。
         */
        tcp_header->magic = '$';
        tcp_header->rtp_len16 = htons((uint16_t)(RTP_HEADER_SIZE + payload_size));
        tcp_header->channel = sig_0;
        ret = sendWithTimeout(sd, (const char *)tcp_header, sizeof(struct rtp_tcp_header), 0);
        if (ret <= 0) {
            return -1;
        }
    }

    /* RTP 头里的多字节字段必须按网络字节序发送。 */
    rtp_packet->rtpHeader.seq = htons(rtp_packet->rtpHeader.seq);
    rtp_packet->rtpHeader.timestamp = htonl(rtp_packet->rtpHeader.timestamp);
    rtp_packet->rtpHeader.ssrc = htonl(rtp_packet->rtpHeader.ssrc);

    if (tcp_header != NULL && sig_0 != -1) {
        ret = sendWithTimeout(sd, (const char *)rtp_packet, RTP_HEADER_SIZE + payload_size, 0);
    } else if (client_rtp_port != -1 && client_ip != NULL) {
        ret = sendUDP(sd, (const char *)rtp_packet, RTP_HEADER_SIZE + payload_size, client_ip, client_rtp_port, 0);
    } else {
        printf("parameter error\n");
        ret = -1;
    }

    /* 发送完成后恢复主机字节序，方便下一包继续递增和复用同一个结构体。 */
    rtp_packet->rtpHeader.seq = ntohs(rtp_packet->rtpHeader.seq);
    rtp_packet->rtpHeader.timestamp = ntohl(rtp_packet->rtpHeader.timestamp);
    rtp_packet->rtpHeader.ssrc = ntohl(rtp_packet->rtpHeader.ssrc);

    if (ret <= 0) {
        return -1;
    }
    rtp_packet->rtpHeader.seq++;
    return ret;
}

static int h264_send_nalu(socket_t sd,
                          struct rtp_tcp_header *tcp_header,
                          struct RtpPacket *rtp_packet,
                          const uint8_t *nalu,
                          uint32_t nalu_size,
                          uint32_t rtp_timestamp,
                          int nalu_is_access_unit_last,
                          int sig_0,
                          char *client_ip,
                          int client_rtp_port,
                          struct RtcpPacketInfo *rtcp_info)
{
    uint8_t fu_indicator;
    uint8_t fu_header_base;
    uint32_t pos;
    int send_bytes = 0;

    if (!nalu || nalu_size == 0) {
        return -1;
    }

    if (nalu_size <= PTK_RTP_TCP_MAX) {
        /*
         * 单 NALU 包：
         * RTP payload 直接放完整 NALU，不包含 Annex-B 起始码。
         * 只有该 NALU 是整帧最后一个 NALU 时 marker 才为 1。
         */
        int ret = h264_send_rtp_packet(sd,
                                       tcp_header,
                                       rtp_packet,
                                       nalu,
                                       nalu_size,
                                       rtp_timestamp,
                                       nalu_is_access_unit_last,
                                       sig_0,
                                       client_ip,
                                       client_rtp_port);
        if (ret <= 0) {
            return -1;
        }
        if (rtcp_info) {
            rtcp_info->packet_count++;
            rtcp_info->octet_count += nalu_size;
        }
        return ret;
    }

    /*
     * FU-A 分片：
     * 原 NALU 第 1 字节不进入 FU payload，而是拆成 FU indicator/header。
     * FU indicator 保留原 F/NRI，Type 改为 28(FU-A)。
     * FU header 保留原 Type，并按分片位置设置 S/E 位。
     */
    fu_indicator = (uint8_t)((nalu[0] & 0xE0) | 28);
    fu_header_base = (uint8_t)(nalu[0] & 0x1F);
    pos = 1;
    while (pos < nalu_size) {
        uint32_t remain = nalu_size - pos;
        uint32_t frag_size = remain > PTK_RTP_TCP_MAX ? PTK_RTP_TCP_MAX : remain;
        uint8_t fu_payload[PTK_RTP_TCP_MAX + 2];
        int is_start = (pos == 1);
        int is_end = (pos + frag_size >= nalu_size);
        int marker = is_end && nalu_is_access_unit_last;
        int ret;

        /* 只有整帧最后一个 NALU 的最后一个 FU-A 分片 marker 才为 1。 */
        fu_payload[0] = fu_indicator;
        fu_payload[1] = fu_header_base;
        if (is_start) {
            fu_payload[1] |= 0x80;
        }
        if (is_end) {
            fu_payload[1] |= 0x40;
        }
        memcpy(fu_payload + 2, nalu + pos, frag_size);

        ret = h264_send_rtp_packet(sd,
                                   tcp_header,
                                   rtp_packet,
                                   fu_payload,
                                   frag_size + 2,
                                   rtp_timestamp,
                                   marker,
                                   sig_0,
                                   client_ip,
                                   client_rtp_port);
        if (ret <= 0) {
            return -1;
        }
        send_bytes += ret;
        if (rtcp_info) {
            rtcp_info->packet_count++;
            rtcp_info->octet_count += frag_size + 2;
        }
        pos += frag_size;
    }
    return send_bytes;
}

int rtpSendH264Frame(socket_t sd,
                     struct rtp_tcp_header *tcp_header,
                     struct RtpPacket *rtp_packet,
                     uint8_t *frame,
                     uint32_t frame_size,
                     uint32_t rtp_timestamp,
                     int sig_0,
                     char *client_ip,
                     int client_rtp_port,
                     struct RtcpPacketInfo *rtcp_info)
{
    uint32_t nalu_start = 0;
    int code_len = 0;
    int total_bytes = 0;

    if (!frame || frame_size == 0 || !rtp_packet) {
        return -1;
    }

    if (rtcp_info) {
        /*
         * RTCP Sender Report 会把 NTP 时间和 RTP timestamp 关联起来。
         * 这里记录的 RTP timestamp 必须来自该帧 PTS 的换算结果，
         * 否则客户端无法基于 SR 做音视频同步和抖动缓冲校准。
         */
        rtcp_info->rtp_timestamp = rtp_timestamp;
        rtcp_info->packet_count = 0;
        rtcp_info->octet_count = 0;
        rtcp_info->ntp_timestamp = getNtpTimestamp64();
        rtcp_info->wallclock_ms = getTimeMs();
    }

    if (h264_find_start_code(frame, frame_size, 0, &nalu_start, &code_len) != 0) {
        /* 兼容路径：输入已经是一个不带起始码的原始 NALU。 */
        return h264_send_nalu(sd,
                              tcp_header,
                              rtp_packet,
                              frame,
                              frame_size,
                              rtp_timestamp,
                              1,
                              sig_0,
                              client_ip,
                              client_rtp_port,
                              rtcp_info);
    }

    while (nalu_start < frame_size) {
        uint32_t payload_start = nalu_start + (uint32_t)code_len;
        uint32_t next_start = frame_size;
        int next_code_len = 0;
        int is_last;
        int ret;

        if (payload_start >= frame_size) {
            break;
        }
        h264_find_start_code(frame, frame_size, payload_start, &next_start, &next_code_len);
        is_last = (next_start >= frame_size);
        if (next_start > payload_start) {
            /*
             * 同一个 Annex-B 帧内的所有 NALU 复用同一个 RTP timestamp。
             * 例如一帧关键帧里可能包含 SPS/PPS/IDR，SPS/PPS 的 RTP marker
             * 仍然为 0，直到最后一个 slice 或最后一个 FU-A 分片才 marker=1。
             */
            ret = h264_send_nalu(sd,
                                 tcp_header,
                                 rtp_packet,
                                 frame + payload_start,
                                 next_start - payload_start,
                                 rtp_timestamp,
                                 is_last,
                                 sig_0,
                                 client_ip,
                                 client_rtp_port,
                                 rtcp_info);
            if (ret <= 0) {
                return -1;
            }
            total_bytes += ret;
        }
        if (is_last) {
            break;
        }
        nalu_start = next_start;
        code_len = next_code_len;
    }
    return total_bytes;
}
