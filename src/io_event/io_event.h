/*** 
 * @Author: huangkelong
 * @Date: 2026-03-02 23:14:35
 * @LastEditTime: 2026-04-19 10:05:56
 * @LastEditors: huangkelong
 * @Description: io事件相关接口定义
 * @FilePath: \Fork\simple-rtsp-server\src\io_event\io_event.h
 * @
 */
#ifndef _IO_EVENT_H_
#define _IO_EVENT_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>
#if defined(__linux__) || defined(__linux)
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <windows.h>
#endif

#include "mthread.h"
#include "socket_io.h"

/** 文件描述符类型 */
typedef enum
{
    FD_TYPE_TCP,      // TCP 控制连接（RTSP 或 RTP-over-TCP）
    FD_TYPE_UDP_RTP,  // UDP RTP 媒体数据
    FD_TYPE_UDP_RTCP, // UDP RTCP 反馈数据
} fd_type_t;

/* 事件数据结构，包含用户数据、文件描述符类型、文件描述符和事件类型。 */
typedef struct
{
    void *user_data;
    fd_type_t fd_type;
    socket_t fd;
    int events;
} event_data_ptr_t;
enum event_type
{
#if defined(__linux__) || defined(__linux)
    EVENT_NONE   = 0,
	EVENT_IN     = EPOLLIN,
	EVENT_PRI    = EPOLLPRI,		
	EVENT_OUT    = EPOLLOUT,
	EVENT_ERR    = EPOLLERR,
	EVENT_HUP    = EPOLLHUP,
	EVENT_RDHUP  = EPOLLRDHUP
#elif defined(_WIN32) || defined(_WIN64)
    EVENT_NONE   = 0,
    EVENT_IN     = 1,
    EVENT_PRI    = 2,
    EVENT_OUT    = 4,
    EVENT_ERR    = 8,
    EVENT_HUP    = 16,
    EVENT_RDHUP  = 8192

#endif
};
typedef int (*event_callback_t)(event_data_ptr_t *);

/* 事件到来后的回调函数 */
typedef struct {
    event_callback_t event_in;
    event_callback_t event_out;
    event_callback_t event_close;
} event_callbacks_t;

int createEvent();
void setEventCallback(event_callback_t event_in, event_callback_t event_out, event_callback_t event_close);
int closeEvent();
int addEvent(int events, event_data_ptr_t *event_data);
int delEvent(event_data_ptr_t *event_data);
void *startEventLoop(void *arg);
void stopEventLoop();

#endif
