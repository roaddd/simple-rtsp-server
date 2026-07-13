#ifndef _RTP_H_
#define _RTP_H_
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "socket_io.h"

struct RtcpPacketInfo
{
    uint32_t rtp_timestamp;
    uint32_t packet_count;
    uint32_t octet_count;
    uint64_t ntp_timestamp;
    uint64_t wallclock_ms;
};

typedef struct RtpPacer {
    int rate_bps;                  /* RTP 视频包 pacing 目标码率，<=0 表示关闭 pacing。 */
    uint64_t next_send_ts_us;      /* 下一 RTP 包允许发送的单调时钟时间，单位微秒。 */
    /* pacing 只依赖单调时钟，避免系统 RTC 校时导致发送间隔突然跳变。 */
} RtpPacer;

/**
 * @description: 初始化 RTP pacer 状态。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 */
void rtpPacerInit(RtpPacer *pacer);

/**
 * @description: 设置 RTP pacer 目标码率。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 * @param rate_bps 目标发送码率，单位 bit/s；<=0 表示关闭 pacing。
 */
void rtpPacerSetRate(RtpPacer *pacer, int rate_bps);

/**
 * @description: 在发送一个 RTP 包前执行 pacing 等待。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 * @param packet_bytes 本次 RTP 包的网络侧字节数，包含 RTP 头和 payload。
 */
void rtpPacerBeforeSend(RtpPacer *pacer, uint32_t packet_bytes);

int rtpSendH264Frame(socket_t sd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    uint8_t *frame, uint32_t frame_size, uint32_t rtp_timestamp, int sig_0, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info, RtpPacer *pacer);
int rtpSendH265Frame(socket_t sd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    uint8_t *frame, uint32_t frame_size, uint32_t rtp_timestamp, int sig_0, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info, RtpPacer *pacer);

int rtpSendAACFrame(socket_t fd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    char *data, int size, uint32_t rtp_timestamp, int channels, int profile, int sig, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);
int rtpSendPCMAFrame(socket_t fd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    char *data, int size, uint32_t rtp_timestamp, int channels, int profile, int sig, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);

#endif
