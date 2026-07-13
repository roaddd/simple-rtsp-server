#include "rtp.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#define RTP_PACER_MAX_SLEEP_US 200000U

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
}
