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

typedef struct RtpPacerStats {
    int rate_bps;                  /* 当前 pacer 目标码率，单位 bit/s。 */
    uint64_t packet_count;         /* pacer 观察到的视频 RTP 包数量。 */
    uint64_t byte_count;           /* pacer 观察到的视频 RTP 包字节数。 */
    uint64_t sleep_count;          /* pacer 实际 sleep 的次数。 */
    uint64_t total_sleep_us;       /* pacer 累计 sleep 时间。 */
    uint32_t last_packet_bytes;    /* 最近一次视频 RTP 包字节数。 */
    uint32_t last_sleep_us;        /* 最近一次发送前 sleep 时间。 */
    uint32_t last_interval_us;     /* 最近一次按包大小计算出的发送间隔。 */
    uint64_t last_send_ts_us;      /* 最近一次允许发送的单调时间。 */
    uint64_t window_start_ts_us;   /* 当前 100ms 统计窗口起始时间。 */
    uint64_t window_bytes;         /* 当前 100ms 统计窗口内 RTP 字节数。 */
    uint32_t window_packets;       /* 当前 100ms 统计窗口内 RTP 包数。 */
    uint64_t last_window_bps;      /* 最近完成的 100ms 窗口估算发送码率。 */
    uint64_t max_window_bps;       /* pacer 启用以来观察到的最大 100ms 窗口码率。 */
    uint64_t current_window_bps;     /* 当前未完成窗口按已用时间估算的码率。 */
    uint64_t last_window_bytes;      /* 最近完成窗口内 RTP 字节数。 */
    uint32_t last_window_packets;    /* 最近完成窗口内 RTP 包数。 */
    uint64_t last_window_elapsed_us; /* 最近完成窗口持续时间。 */
    uint64_t max_window_bytes;       /* 最大码率窗口内 RTP 字节数。 */
    uint32_t max_window_packets;     /* 最大码率窗口内 RTP 包数。 */
    uint64_t max_window_elapsed_us;  /* 最大码率窗口持续时间。 */
    uint64_t reset_count;            /* pacer 时间基准被重置的次数。 */
    uint32_t last_reset_reason;      /* 最近一次重置原因：1=首次/未初始化，2=落后超过保护阈值。 */
    uint64_t last_reset_lag_us;      /* 最近一次落后重置时，当前时间超过计划发送时间的微秒数。 */
    uint64_t last_reset_now_us;      /* 最近一次重置发生时的单调时间。 */
    uint64_t last_reset_next_send_ts_us; /* 最近一次重置前的计划发送时间。 */
    uint64_t last_reset_window_bytes; /* 最近一次重置时当前统计窗口内已经发送的 RTP 字节数。 */
    uint32_t last_reset_window_packets; /* 最近一次重置时当前统计窗口内已经发送的 RTP 包数。 */
} RtpPacerStats;

typedef struct RtpPacer {
    RtpPacerStats stats;           /* RTP pacer 调试统计，用于排查是否真正平滑发送。 */
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
void rtpPacerGetStats(RtpPacer *pacer, RtpPacerStats *stats);

int rtpSendH264Frame(socket_t sd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    uint8_t *frame, uint32_t frame_size, uint32_t rtp_timestamp, int sig_0, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info, RtpPacer *pacer);
int rtpSendH265Frame(socket_t sd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    uint8_t *frame, uint32_t frame_size, uint32_t rtp_timestamp, int sig_0, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info, RtpPacer *pacer);

int rtpSendAACFrame(socket_t fd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    char *data, int size, uint32_t rtp_timestamp, int channels, int profile, int sig, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);
int rtpSendPCMAFrame(socket_t fd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    char *data, int size, uint32_t rtp_timestamp, int channels, int profile, int sig, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);

#endif
