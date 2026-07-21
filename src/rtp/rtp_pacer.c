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

    clock_gettime(CLOCK_MONOTONIC, &ts); /* 单调时钟不受系统时间校时影响。 */
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
 * @description: 设置 RTP pacer 目标发送码率。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 * @param rate_bps 目标发送码率，单位 bit/s，<=0 表示关闭 pacing。
 */
void rtpPacerSetRate(RtpPacer *pacer, int rate_bps)
{
    if (!pacer)
    {
        LOG_ERROR("rtpPacerSetRate: pacer is null");
        return;
    }

    if (rate_bps <= 0)
    {
        LOG_ERROR("rtpPacerSetRate: rate_bps is <= 0");
        pacer->rate_bps = 0;
        pacer->next_send_ts_us = 0;
        pacer->stats.rate_bps = 0;
        return;
    }

    /*
     * 码率变化时不重置 next_send_ts_us。
     * 如果直接用当前时间重新起步，下一包可能绕过旧时间基准立即发送，反而制造一次突发。
     */
    pacer->rate_bps = rate_bps;
    pacer->stats.rate_bps = rate_bps;
}

/**
 * @description: 更新 RTP pacer 100ms 发送统计窗口。
 * @param pacer 每个客户端独立持有的 pacer 状态。
 * @param packet_bytes 本次 RTP 包字节数。
 * @param sleep_us 本次发送前实际等待的微秒数。
 * @param packet_interval_us 按当前 pacing rate 计算出的本包发送间隔。
 * @param send_ts_us 本次实际发送前的时间戳。
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
        /*
         * 完成窗口用真实 elapsed_us 计算，而不是固定除以 100ms。
         * 这样可以判断 9Mbps 峰值到底是窗口统计偏差，还是 pacer 确实放出了更多字节。
         */
        if (elapsed_us > 0)
        {
            window_bps = pacer->stats.window_bytes * 8ULL * 1000000ULL / elapsed_us;
            pacer->stats.last_window_bps = window_bps;
            pacer->stats.last_window_bytes = pacer->stats.window_bytes;
            pacer->stats.last_window_packets = pacer->stats.window_packets;
            pacer->stats.last_window_elapsed_us = elapsed_us;
            if (window_bps > pacer->stats.max_window_bps)
            {
                pacer->stats.max_window_bps = window_bps;
                pacer->stats.max_window_bytes = pacer->stats.window_bytes;
                pacer->stats.max_window_packets = pacer->stats.window_packets;
                pacer->stats.max_window_elapsed_us = elapsed_us;
            }
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

    elapsed_us = send_ts_us >= pacer->stats.window_start_ts_us ?
                 send_ts_us - pacer->stats.window_start_ts_us :
                 0;
    if (elapsed_us > 0)
        pacer->stats.current_window_bps = pacer->stats.window_bytes * 8ULL * 1000000ULL / elapsed_us;
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
    if (pacer->next_send_ts_us == 0)
    {
        pacer->next_send_ts_us = now_us;
        pacer->stats.reset_count++;
        pacer->stats.last_reset_reason = 1; /* 首包或 pacer 被重新打开。 */
    }
    else if (pacer->next_send_ts_us + RTP_PACER_MAX_SLEEP_US < now_us)
    {
        /*
         * 发送线程已经明显落后于计划时间，继续追旧时间没有意义。
         * 这里重置时间基准，并记录原因，用于排查是否因为重置导致瞬时突发。
         */
        pacer->next_send_ts_us = now_us;
        pacer->stats.reset_count++;
        pacer->stats.last_reset_reason = 2; /* 落后超过保护阈值。 */
    }

    if (pacer->next_send_ts_us > now_us)
    {
        sleep_us = pacer->next_send_ts_us - now_us;
        if (sleep_us > RTP_PACER_MAX_SLEEP_US)
            sleep_us = RTP_PACER_MAX_SLEEP_US;
        actual_sleep_us = (uint32_t)sleep_us;
        usleep((useconds_t)sleep_us); /* 当前实现依赖系统 sleep 调度，后续可替换为令牌桶。 */
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
 * @param pacer 每个客户端独立持有的 pacer 状态。
 * @param stats 输出 pacer 调试统计快照。
 */
void rtpPacerGetStats(RtpPacer *pacer, RtpPacerStats *stats)
{
    if (!pacer || !stats)
        return;
    *stats = pacer->stats;
}
