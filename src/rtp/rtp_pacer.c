#include "rtp.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#define RTP_PACER_MAX_SLEEP_US 200000U
#define RTP_PACER_STATS_WINDOW_US 100000U

/**
 * @description: 获取 RTP pacer 使用的单调时钟，单位微秒。
 */
static uint64_t rtp_pacer_now_us(void)
{
    struct timespec ts = {0};

    clock_gettime(CLOCK_MONOTONIC, &ts); /* 不会受系统时间校时影响 */
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/**
 * @description: 初始化 RTP pacer 状态。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 */
void rtpPacerInit(RtpPacer *pacer)
{
    if (!pacer)
        return;
    memset(pacer, 0, sizeof(*pacer));
}

/**
 * @description: 设置 RTP pacer 目标码率，码率变化时重置发送基准时间。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 * @param rate_bps 目标发送码率，单位 bit/s，<=0 表示关闭 pacing。
 */
void rtpPacerSetRate(RtpPacer *pacer, int rate_bps)
{
    if (!pacer)
        return;
    if (rate_bps <= 0)
    {
        pacer->rate_bps = 0;
        pacer->next_send_ts_us = 0;
        pacer->stats.rate_bps = 0;
        return;
    }
    if (pacer->rate_bps != rate_bps)
    {
        /*
         * 码率变化时从当前时间重新起步，避免沿用旧码率下积累的发送时间导致
         * 第一个包被异常延迟。
         */
        pacer->next_send_ts_us = rtp_pacer_now_us();
    }
    pacer->rate_bps = rate_bps;
    pacer->stats.rate_bps = rate_bps;
}

/**
 * @description: 更新 RTP pacer 100ms 统计窗口。
 */
static void rtp_pacer_update_stats(RtpPacer *pacer,
                                   uint32_t packet_bytes,
                                   uint32_t sleep_us,
                                   uint32_t packet_interval_us,
                                   uint64_t send_ts_us)
{
    uint64_t elapsed_us = 0;
    uint64_t window_bps = 0;

    if (!pacer)
        return;

    if (pacer->stats.window_start_ts_us == 0)
        pacer->stats.window_start_ts_us = send_ts_us;

    elapsed_us = send_ts_us >= pacer->stats.window_start_ts_us ?
                 send_ts_us - pacer->stats.window_start_ts_us :
                 0;
    if (elapsed_us >= RTP_PACER_STATS_WINDOW_US)
    {
        if (elapsed_us > 0)
        {
            window_bps = pacer->stats.window_bytes * 8ULL * 1000000ULL / elapsed_us;
            pacer->stats.last_window_bps = window_bps;
            if (window_bps > pacer->stats.max_window_bps)
                pacer->stats.max_window_bps = window_bps;
        }
        pacer->stats.window_start_ts_us = send_ts_us;
        pacer->stats.window_bytes = 0;
        pacer->stats.window_packets = 0;
    }

    pacer->stats.rate_bps = pacer->rate_bps;
    pacer->stats.packet_count++;
    pacer->stats.byte_count += packet_bytes;
    if (sleep_us > 0)
    {
        pacer->stats.sleep_count++;
        pacer->stats.total_sleep_us += sleep_us;
    }
    pacer->stats.last_packet_bytes = packet_bytes;
    pacer->stats.last_sleep_us = sleep_us;
    pacer->stats.last_interval_us = packet_interval_us;
    pacer->stats.last_send_ts_us = send_ts_us;
    pacer->stats.window_bytes += packet_bytes;
    pacer->stats.window_packets++;
}

/**
 * @description: 在发送一个 RTP 包前按目标码率等待，削平大帧分片突发。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 * @param packet_bytes 本次 RTP 包的网络侧字节数，包含 RTP 头和 payload。
 */
void rtpPacerBeforeSend(RtpPacer *pacer, uint32_t packet_bytes)
{
    uint64_t now_us = 0;
    uint64_t sleep_us = 0;
    uint64_t packet_interval_us = 0;
    uint32_t actual_sleep_us = 0;

    if (!pacer || pacer->rate_bps <= 0 || packet_bytes == 0)
        return;

    now_us = rtp_pacer_now_us();
    if (pacer->next_send_ts_us == 0 || pacer->next_send_ts_us + RTP_PACER_MAX_SLEEP_US < now_us)
        pacer->next_send_ts_us = now_us;

    if (pacer->next_send_ts_us > now_us)
    {
        sleep_us = pacer->next_send_ts_us - now_us;
        if (sleep_us > RTP_PACER_MAX_SLEEP_US)
            sleep_us = RTP_PACER_MAX_SLEEP_US;
        actual_sleep_us = (uint32_t)sleep_us;
        usleep((useconds_t)sleep_us);  /* TODO:不用sleep的方案？ */
        now_us = rtp_pacer_now_us();
        if (pacer->next_send_ts_us < now_us)
            pacer->next_send_ts_us = now_us;
    }

    /*
     * 按“本包字节数 * 8 / 目标 bit/s”推进下一包发送时间。
     * +rate_bps-1 用于向上取整，避免小包间隔被截断为 0。
     */
    packet_interval_us = ((uint64_t)packet_bytes * 8ULL * 1000000ULL +
                          (uint64_t)pacer->rate_bps - 1ULL) /
                         (uint64_t)pacer->rate_bps;
    if (packet_interval_us == 0)
        packet_interval_us = 1;
    pacer->next_send_ts_us += packet_interval_us;
    rtp_pacer_update_stats(pacer,
                           packet_bytes,
                           actual_sleep_us,
                           (uint32_t)packet_interval_us,
                           now_us);
}

/**
 * @description: 读取 RTP pacer 调试统计快照。
 */
void rtpPacerGetStats(RtpPacer *pacer, RtpPacerStats *stats)
{
    if (!pacer || !stats)
        return;
    *stats = pacer->stats;
}
