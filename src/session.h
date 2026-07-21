#ifndef _SESSION_H_
#define _SESSION_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>

#include "common.h"
#include "rtp.h"
#include "socket_io.h"
#include "media.h"
#include "rtsp_message.h"
#include "io_event.h"
#include "mthread.h"

#define CLIENTMAX           512
#define FILEMAX             512
#define VIDEO_DATA_MAX_SIZE 2 * 1024 * 1024
#define RING_BUFFER_MAX     32
// #define SEND_DATA_EVENT // If using event to send audio and video, the memory grows rapidly and needs to be fixed

/**
 * 当前每调一次 sessionSendVideoFrame()，rtsp-server 就直接对每个正在 PLAY 的 client 发送 RTP
 */

enum TRANSPORT_e
{
    RTP_OVER_TCP = 1,
    RTP_OVER_UDP,
};
enum MEDIA_e
{
    VIDEO = 1,
    AUDIO,
};

#ifndef RTSP_VIDEO_PACER_MODE_DEFINED
#define RTSP_VIDEO_PACER_MODE_DEFINED
/*
 * RTSP session 视频 RTP pacer 开关状态。
 * 使用枚举替代裸 int，避免调用层把开关值和 pacing rate 混用。
 */
typedef enum {
    RTSP_VIDEO_PACER_DISABLED = 0, /* 关闭视频 RTP 包级 pacer。 */
    RTSP_VIDEO_PACER_ENABLED = 1   /* 开启视频 RTP 包级 pacer。 */
} RtspVideoPacerMode;
#endif

#ifndef RTSP_VIDEO_PACER_STATS_DEFINED
#define RTSP_VIDEO_PACER_STATS_DEFINED
#define RTSP_VIDEO_PACER_STATS_MAX_CLIENTS 16
typedef struct {
    int in_use;                    /* 客户端槽位是否正在使用。 */
    char client_ip[40];            /* 客户端 IP。 */
    int client_rtp_port;           /* 客户端视频 RTP 端口。 */
    int transport;                 /* RTSP transport 类型。 */
    int rate_bps;                  /* 当前客户端 pacer 目标码率。 */
    uint64_t packet_count;         /* pacer 观察到的视频 RTP 包数量。 */
    uint64_t byte_count;           /* pacer 观察到的视频 RTP 字节数。 */
    uint64_t sleep_count;          /* pacer 实际 sleep 次数。 */
    uint64_t total_sleep_us;       /* pacer 累计 sleep 时间。 */
    uint32_t last_packet_bytes;    /* 最近一次 RTP 包字节数。 */
    uint32_t last_sleep_us;        /* 最近一次 sleep 时间。 */
    uint32_t last_interval_us;     /* 最近一次按 rate 计算的发送间隔。 */
    uint64_t last_window_bps;      /* 最近完成的 100ms 窗口估算码率。 */
    uint64_t max_window_bps;       /* 已观察到的最大 100ms 窗口估算码率。 */
    uint64_t current_window_bps;   /* 当前未完成窗口按已用时间估算的码率。 */
    uint64_t last_window_bytes;    /* 最近完成窗口内 RTP 字节数。 */
    uint32_t last_window_packets;  /* 最近完成窗口内 RTP 包数。 */
    uint64_t last_window_elapsed_us; /* 最近完成窗口持续时间。 */
    uint64_t max_window_bytes;     /* 最大码率窗口内 RTP 字节数。 */
    uint32_t max_window_packets;   /* 最大码率窗口内 RTP 包数。 */
    uint64_t max_window_elapsed_us; /* 最大码率窗口持续时间。 */
    uint64_t reset_count;          /* pacer 时间基准被重置的次数。 */
    uint32_t last_reset_reason;    /* 最近一次重置原因：1=首次/未初始化，2=落后超过保护阈值。 */
} RtspVideoPacerClientStats;

typedef struct {
    RtspVideoPacerMode mode;       /* 当前 session pacer 开关。 */
    int pacing_rate_bps;           /* 当前 session 目标 pacing rate。 */
    int total_client_count;        /* 当前 session 总客户端数量。 */
    int reported_client_count;     /* 本快照实际填充的客户端数量。 */
    RtspVideoPacerClientStats clients[RTSP_VIDEO_PACER_STATS_MAX_CLIENTS];
} RtspVideoPacerStats;
#endif
struct MediaPacket_st
{
    char data[2 * 1024 * 1024];
    int64_t size;
    int type; // MEDIA_e
    uint64_t pts_us;
};

#ifndef RTSP_MEDIA_FRAME_DEFINED
#define RTSP_MEDIA_FRAME_DEFINED
typedef struct RtspMediaFrame {
    uint8_t *data;
    int data_len;
    uint64_t pts_us;
} RtspMediaFrame;
#endif
struct RtcpSenderContext
{
    uint32_t packet_count;
    uint32_t octet_count;
    uint32_t last_rtp_timestamp;
    uint64_t last_ntp_timestamp;
    uint64_t last_sr_time_ms;
};
struct RtcpReceiverContext
{
    uint64_t packet_count;  // 收到 RTCP 包总数
    uint64_t rr_count;      // RR 包计数
    uint64_t sr_count;      // SR 包计数
    uint64_t sdes_count;    // SDES 包计数
    uint64_t bye_count;     // BYE 包计数

    uint32_t last_reportee_ssrc; // 最近一次 RR 报告块里的被报告 SSRC
    uint8_t fraction_lost;       // 分数丢包率(0~255)
    int32_t cumulative_lost;     // 累计丢包(24-bit signed)
    uint32_t highest_seq;        // 扩展最高序列号
    uint32_t jitter;             // 互达抖动
    uint32_t lsr;                // Last SR middle 32 bits
    uint32_t dlsr;               // Delay since Last SR
    uint32_t rtt_ms;             // 估算 RTT(ms)

    uint64_t last_rx_time_ms;    // 最近一次收到 RTCP 的时间戳(ms)
};
/*Record the data channel and packets of the client*/
struct clientinfo_st
{
    socket_t sd;          // client tcp socket
    // video
    socket_t udp_sd_rtp;  // server rtp udp socket
    socket_t udp_sd_rtcp; // server rtcp udp socket
    // audio
    socket_t udp_sd_rtp_1;
    socket_t udp_sd_rtcp_1;

    event_data_ptr_t *event_data[5]; // tcp/rtp1/rtcp1/rtp2/rtcp2

    char client_ip[40];
    int client_rtp_port;
    int client_rtcp_port;
    int client_rtp_port_1;
    int client_rtcp_port_1;

    int transport; // enum TRANSPORT_e

    // RTP_OVER_TCP-->rtp sig
    int sig_0; // video 视频rtp通道号
    int sig_1; // video rtcp 视频rtcp通道号
    int sig_2; // audio 音频rtp通道号
    int sig_3; // audio rtcp 音频rtcp通道号
    int playflag; // 0:not play 1:play

    void (*send_call_back)(void *arg); // Audio and video processing callback function
    int events;                        // EPOLLIN, EPLLOUT, EPOLLERR ,EPOLLRDHUP
    struct session_st *session;        // Point to session structure

    struct RtpPacket *rtp_packet;   // video 
    struct RtpPacket *rtp_packet_1; // audio
    struct rtp_tcp_header *tcp_header;
    RtpPacer video_pacer;              /* 视频 RTP 包级 pacer，每个客户端独立维护发送节奏。 */
    struct RtcpSenderContext rtcp_video;
    struct RtcpSenderContext rtcp_audio;
    struct RtcpReceiverContext rtcp_rx_video; // 视频 RTCP 接收统计
    struct RtcpReceiverContext rtcp_rx_audio; // 音频 RTCP 接收统计

    // Circular buffer queue
    // video
    mthread_mutex_t mut_list;
    struct MediaPacket_st *packet_list;
    int packet_list_size; // Circular buffer queue size
    int pos_list;         // The next location to send data
    int packet_num;       // Number of data packets in the circular buffer queue
    int pos_last_packet;  // The available tail positions for the circular buffer queue

    // audio
    struct MediaPacket_st *packet_list_1;
    int packet_list_size_1;
    int pos_list_1;
    int packet_num_1;
    int pos_last_packet_1;

    // RTCP, RTSP MESSAGE(heartbeat or TEARDOWN)
    char buffer[1024];
    int len;
    int pos;
};
/*rtsp session*/
struct session_st
{
    void *media;
    char *filename;
    mthread_mutex_t mut;
    struct clientinfo_st clientinfo[CLIENTMAX]; // Client connection queue for session
    int count;
    int pos;

    // Custom session
    int is_custom;
    enum VIDEO_e video_type;
    enum AUDIO_e audio_type;
    int profile;
    int sample_rate;
    int channels;
    RtspVideoPacerMode video_pacer_mode; /* 当前 session 是否启用视频 RTP 包级 pacer。 */
    int video_pacing_rate_bps;         /* 当前 session 的视频 RTP pacing 目标码率，单位 bit/s。 */
};
#ifdef RTSP_FILE_SERVER
/**
 * File playback configuration
 * @param[in] file_reloop_flag  Does the file loop back? 0:not 1: is
 * @param[in]  mp4_file_path    The folder path where the file is located, if NULL default:./mp4path
 * @return 0:ok <0:error
 */
int configSession(int file_reloop_flag, const char *mp4_file_path);
#endif
/**
 * Linux signal processing callback function
 */
void sig_handler(int s);
/**
 * initialization, must be called at the beginning of the program
 * @return 0:ok <0:error
 */
int moduleInit();
/**
 * destroy, it can be called or not called. If called, it must be called at the end of the program
 */
void moduleDel();
/**
 * Initialize the client and set default information
 * @return 0:ok <0:error
 */
int initClient(struct session_st *session, struct clientinfo_st *clientinfo);
/**
 * Clear client connection
 * @return 0:ok <0:error
 */
int clearClient(struct clientinfo_st *clientinfo);

void pushFrameToList1(struct clientinfo_st *clientinfo, char *ptr, int ptr_len, int type, uint64_t pts_us);
void pushFrameToList2(struct clientinfo_st *clientinfo, char *ptr, int ptr_len, int type, uint64_t pts_us);

struct MediaPacket_st getFrameFromList1(struct clientinfo_st *clientinfo);
struct MediaPacket_st getFrameFromList2(struct clientinfo_st *clientinfo);

/**
 * Create client RTP connection
 * @return 0:ok <0:error
 */
int createClient(struct clientinfo_st *clientinfo, 
    socket_t client_sock_fd, int sig_0, int sig_1, int sig_2, int sig_3, int ture_of_tcp, /*tcp*/
    socket_t server_rtp_fd, socket_t server_rtcp_fd, socket_t server_rtp_fd_1, socket_t server_rtcp_fd_1, char *client_ip, int client_rtp_port, int client_rtcp_port, int client_rtp_port_1, int client_rtcp_port_1 /*udp*/
    );
#ifdef RTSP_FILE_SERVER
/**
 * add file session
 * @return 0:ok <0:error
 */
int addFileSession(char *path_filename, 
                socket_t client_sock_fd, int sig_0, int sig_1, int sig_2, int sig_3, int ture_of_tcp, /*tcp*/
                socket_t server_rtp_fd, socket_t server_rtcp_fd, socket_t server_rtp_fd_1, socket_t server_rtcp_fd_1, char *client_ip, int client_rtp_port, int client_rtcp_port, int client_rtp_port_1, int client_rtcp_port_1 /*udp*/
                );
/**
 * delete file session
 */
void delFileSession(struct session_st *session);
#endif
/**
 * add custom session(live)
 * @return context
 */
void* addCustomSession(const char* session_name);
/**
 * delete custom session
 */
void delCustomSession(void *context);
/**
 * add video(custom session)
 * @return 0:ok <0:error
 */
int addVideo(void *context, enum VIDEO_e type);
/**
 * add audio(custom session), profile for AAC
 * @return 0:ok <0:error
 */
int addAudio(void *context, enum AUDIO_e type, int profile, int sample_rate, int channels);
/**
 * 设置自定义 session 的视频 RTP 包级 pacer。
 * @param context addCustomSession 返回的 session 指针。
 * @param mode pacer 开关状态。
 * @param pacing_rate_bps 目标码率，单位 bit/s；mode 为 RTSP_VIDEO_PACER_ENABLED 时必须大于 0。
 * @return 0:ok <0:error
 */
int setVideoPacer(void *context, RtspVideoPacerMode mode, int pacing_rate_bps);
int getVideoPacerStats(void *context, RtspVideoPacerStats *stats);
/**
 * 发送一帧编码视频。H264 Annex-B 帧在 RTSP server 内部拆成 RTP NALU/FU-A 包，
 * 并使用 frame->pts_us 作为 RTP timestamp 的换算基准。
 * @return 0:ok <0:error
 */
int sendVideoFrame(void *context, const RtspMediaFrame *frame);
/**
 * 发送一帧编码音频。AAC payload 不含 ADTS；PCMA 为原始 G711A 数据。
 * @return 0:ok <0:error
 */
int sendAudioFrame(void *context, const RtspMediaFrame *frame);

int getSessionAudioType(struct session_st *session);
int getSessionAudioInfo(struct session_st *session, int *sample_rate, int *channels, int *profile);
int getSessionVideoType(struct session_st *session);
/**
 * Does the session exist
 * @return 1:exit 0:not exit
 */
int sessionIsExist(char* suffix);
/**
 * generate SDP
 * @return 0:ok <0:error
 */
int sessionGenerateSDP(char *suffix, char *localIp, char *buffer, int buffer_len);

/**
 * add client
 * @return 0:ok <0:error
 */
int addClient(char* suffix, 
            socket_t client_sock_fd, int sig_0, int sig_1, int sig_2, int sig_3, int ture_of_tcp, /*tcp*/
            char *client_ip, int client_rtp_port, int client_rtcp_port, int client_rtp_port_1, int client_rtcp_port_1, /*client udp info*/
            socket_t server_udp_socket_rtp, socket_t server_udp_socket_rtcp, socket_t server_udp_socket_rtp_1, socket_t server_udp_socket_rtcp_1 /*udp socket*/
            );
/**
 * otal number of client connections
 * @return client numbers
 */
int getClientNum();
int getSessionClientNum(void *context);

#endif
