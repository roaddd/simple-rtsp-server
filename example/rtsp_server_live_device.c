#include "rtsp_server_handle.h"
#include "mthread.h"
#include "device_audio.h"
#include "device_video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SERVER_IP   "0.0.0.0"
#define SERVER_PORT 8554
#define USER        "admin"
#define PASSWORD    "123456"
static void *context;
static int run_flag = 1;
// FILE *output_h264 = NULL;
void print_nalu_type(uint8_t nalu_type) {
    const char *frame_type = "Unknown";
    switch (nalu_type) {
        case 1: frame_type = "Non-IDR Slice"; break;
        case 5: frame_type = "IDR Slice"; break;
        case 6: frame_type = "SEI"; break;
        case 7: frame_type = "SPS"; break;
        case 8: frame_type = "PPS"; break;
        case 9: frame_type = "AUD"; break;
        default: break;
    }
    printf("NALU Type: %d (%s)\n", nalu_type, frame_type);
}
void videoCallback(uint8_t *data, int data_len, void *arg){
    RtspMediaFrame rtsp_frame;
    // if (output_h264 == NULL) {
    //     output_h264 = fopen("output.h264", "wb");
    //     if (!output_h264) {
    //         printf("Failed to open output file\n");
    //         return;
    //     }
    // }

    rtsp_frame.data = data;
    rtsp_frame.data_len = data_len;
    rtsp_frame.pts_us = getTimeMs() * 1000ULL;
    if(sessionSendVideoFrame(context, &rtsp_frame) < 0){
        printf("sessionSendVideoFrame error\n");
    }
}
void *sendVideoDataThd(void *arg){
    if(startVideoDeviceLoop() < 0){
        return NULL;
    }
    return NULL;
}
// FILE *output_aac = NULL;
void audioCallback(uint8_t *data, int data_len, void *arg){
    // if(output_aac == NULL){
    //     output_aac = fopen("output.aac", "wb");
    //     if(!output_aac){
    //         printf("Failed to open output file\n");
    //         return;
    //     }
    // }
    // fwrite(data, 1, data_len, output_aac);
    int adts_header_len = 7;
    RtspMediaFrame rtsp_frame = { data + adts_header_len, data_len - adts_header_len, getTimeMs() * 1000ULL };
    if(sessionSendAudioFrame(context, &rtsp_frame) < 0){
        printf("sessionSendAudioFrame error\n");
    }
}
void *sendAudioDataThd(void *arg){
    if(startAudioDeviceLoop() < 0){
        return NULL;
    }
    return NULL;
}
int main(int argc, char *argv[])
{
    if(argc < 2){
        printf("./rtsp_server_live auth(0-not authentication; 1-authentication)\n");
        return -1;
    }
    int auth = atoi(argv[1]);
    int ret = rtspModuleInit();
    if(ret < 0){
        printf("rtspModuleInit error\n");
        return -1;
    }
    // add custom session
    context = rtspAddSession("live");
    if(context == NULL){
        printf("rtspAddSession error\n");
        return -1;
    }
    ret = sessionAddVideo(context, VIDEO_H264);
    if(ret < 0){
        printf("sessionAddVideo error\n");
        return -1;
    }
    ret = sessionAddAudio(context, AUDIO_AAC, 1, SAMPLE_RATE, CHANNELS);
    if(ret < 0){
        printf("sessionAddAudio error\n");
        return -1;
    }
    printf("rtsp://%s:%d/live\n", SERVER_IP, SERVER_PORT);

    setVideoCallback(videoCallback, NULL);
    mthread_t tid_v;
    ret = mthread_create(&tid_v, NULL, sendVideoDataThd, NULL);
    if(ret < 0){
        printf("sendVideoDataThd mthread_create()\n");
        return -1;
    }
    mthread_detach(tid_v);

    setAudioCallback(audioCallback, NULL);
    mthread_t tid_a;
    ret = mthread_create(&tid_a, NULL, sendAudioDataThd, NULL);
    if(ret < 0){
        printf("sendAudioDataThd mthread_create()\n");
        return -1;
    }
    mthread_detach(tid_a);
    ret = rtspStartServer(auth, SERVER_IP, SERVER_PORT, USER, PASSWORD);
    if(ret < 0){
        printf("rtspStartServer error\n");
    }

    run_flag = 0;
    stopVideoDevice();
    stopAudioDevice();

    rtspStopServer();
    rtspDelSession(context);
    rtspModuleDel();
    return 0;
}
