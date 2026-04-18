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

int sessionSendVideoData(void *context, uint8_t *data, int data_len);
int sessionSendAudioData(void *context, uint8_t *data, int data_len);

#ifdef __cplusplus
}
#endif

#endif
