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

int rtpSendH264Frame(socket_t sd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    uint8_t *frame, uint32_t frame_size, int fps, int sig_0, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);
int rtpSendH265Frame(socket_t sd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    uint8_t *frame, uint32_t frame_size, int fps, int sig_0, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);

int rtpSendAACFrame(socket_t fd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    char *data, int size, uint32_t sample_rate, int channels, int profile, int sig, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);
int rtpSendPCMAFrame(socket_t fd, struct rtp_tcp_header *tcp_header, struct RtpPacket *rtp_packet, 
                    char *data, int size, uint32_t sample_rate, int channels, int profile, int sig, char *client_ip, int client_rtp_port, struct RtcpPacketInfo *rtcp_info);

#endif