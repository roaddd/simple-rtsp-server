/**
 * @file io_event.h
 * @brief IO 事件轮询和线程唤醒接口。
 *
 * 本文件封装 RTSP server 内部的跨平台 socket 事件等待能力。
 * 上层通过 io_event 注册 socket，事件线程负责等待可读/可写事件并唤醒处理逻辑。
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
typedef struct event_data_ptr_st
{
    void *user_data;
    fd_type_t fd_type;
    socket_t fd;
    int events;
    /* event_data 会被 epoll/select 的本轮事件快照继续引用，不能在回调中立即释放。 */
    int active;
    /* 标记已从监听集合移除，等待 event loop 当前批次处理结束后再统一释放。 */
    int pending_free;
    struct event_data_ptr_st *next_pending_free;
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
/* 延迟释放 event_data，避免同一批 epoll_wait 返回的后续事件访问已释放指针。 */
void retireEventData(event_data_ptr_t *event_data);
void *startEventLoop(void *arg);
void stopEventLoop();

#endif
