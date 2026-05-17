/*** 
 * @Author: huangkelong
 * @Date: 2026-03-03 23:10:24
 * @LastEditTime: 2026-04-16 23:38:37
 * @LastEditors: huangkelong
 * @Description: rtsp server对外头文件
 * @FilePath: \Fork\simple-rtsp-server\include\rtsp_server_api.h
 * @可以输入预定的版权声明、个性签名、空行等
 */
#ifndef _RTSP_SERVER_API_H_
#define _RTSP_SERVER_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum VIDEO_e
{
    VIDEO_H264 = 1,
    VIDEO_H265,
    VIDEO_NONE,
};

enum AUDIO_e
{
    AUDIO_AAC = 1,
    AUDIO_PCMA,
    AUDIO_NONE,
};

#ifndef RTSP_MEDIA_FRAME_DEFINED
#define RTSP_MEDIA_FRAME_DEFINED
/*
 * 上游媒体流水线传入的一帧编码数据。
 * data/data_len 指向完整帧缓存。H264 Annex-B 帧内可以包含多个 NALU；
 * RTSP server 负责在内部做 RTP 打包。
 * pts_us 是媒体显示时间戳，单位微秒，不是 RTP tick。
 */
typedef struct RtspMediaFrame {
    uint8_t *data;
    int data_len;
    uint64_t pts_us;
} RtspMediaFrame;
#endif

int rtspModuleInit(void);
void rtspModuleDel(void);

int rtspConfigSession(int file_reloop_flag, const char *mp4_file_path);

/*** 
 * @description: 在“已经启动的 RTSP server”里注册一条流路径
 * @param {char*} session_name
 * @return {*}
 */
void* rtspAddSession(const char* session_name);
void rtspDelSession(void *context);

int rtspStartServer(int auth, const char *server_ip, int server_port, const char *user, const char *password);
void rtspStopServer(void);

int sessionAddVideo(void *context, enum VIDEO_e type);
int sessionAddAudio(void *context, enum AUDIO_e type, int profile, int sample_rate, int channels);

/*
 * 整帧发送 API。server 会把 frame->pts_us 换算到对应媒体的 RTP 时钟域，
 * 并保证同一帧拆出的所有 RTP 包使用同一个 timestamp。
 */
int sessionSendVideoFrame(void *context, const RtspMediaFrame *frame);
int sessionSendAudioFrame(void *context, const RtspMediaFrame *frame);
int rtspSessionGetClientNum(void *context);

#ifdef __cplusplus
}
#endif

#endif
