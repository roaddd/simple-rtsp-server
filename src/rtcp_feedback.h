#ifndef _RTCP_FEEDBACK_H_
#define _RTCP_FEEDBACK_H_

#include <stdint.h>

struct clientinfo_st;

/**
 * @description: 处理 RTP-over-TCP 交织通道中的 RTCP 包并更新统计
 * @param {struct clientinfo_st} *clientinfo 客户端上下文
 * @param {uint8_t} channel 交织通道号（通常对应 sig_1/sig_3）
 * @param {uint8_t} *payload RTCP 负载
 * @param {int} payload_len RTCP 负载长度
 */
void rtcpHandleInterleaved(struct clientinfo_st *clientinfo, uint8_t channel, const uint8_t *payload, int payload_len);
/**
 * @description: 处理 UDP RTCP socket 收到的 RTCP 包并更新统计
 * @param {struct clientinfo_st} *clientinfo 客户端上下文
 * @param {int} is_audio 0:视频 1:音频
 * @param {uint8_t} *payload RTCP 负载
 * @param {int} payload_len RTCP 负载长度
 */
void rtcpHandleUdp(struct clientinfo_st *clientinfo, int is_audio, const uint8_t *payload, int payload_len);

#endif
