#include "io_event.h"

#include "logger.h"
#if defined(__linux__) || defined(__linux)
#define EVENT_DEBUG
#define EPOLL_MAX   1024
static int epoll_fd;
static int run_flag = 1;
static int event_listen_cnt = 0;
static mthread_mutex_t mut_epoll;
event_callbacks_t event_callbacks = {NULL, NULL, NULL};
void setEventCallback(event_callback_t event_in, event_callback_t event_out, event_callback_t event_close){
    event_callbacks.event_in = event_in;
    event_callbacks.event_out = event_out;
    event_callbacks.event_close = event_close;
    return;
}
int createEvent(){
    epoll_fd = epoll_create(EPOLL_MAX);
    if(epoll_fd < 0){
        printf("create efd in %s err %d\n", __func__, epoll_fd);
        return -1;
    }
    mthread_mutex_init(&mut_epoll, NULL);
    return 0;
}
int closeEvent(){
    if(epoll_fd >= 0){
        close(epoll_fd);
    }
    mthread_mutex_destroy(&mut_epoll);
    return 0;
}
int addEvent(int events, event_data_ptr_t *event_data){
    if(event_data == NULL){
        LOG_ERROR("[EVENT_ADD] event_data is null listen_cnt=%d", event_listen_cnt);
        return -1;
    }
    mthread_mutex_lock(&mut_epoll);
    struct epoll_event epv = {0, {0}};
    epv.data.ptr = event_data;
    event_data->events = events;
    epv.events = events;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_data->fd, &epv) < 0){
        LOG_ERROR("[EVENT_ADD] failed fd=%d fd_type=%d events=0x%x listen_cnt=%d event_data=%p user_data=%p",
                  event_data->fd, event_data->fd_type, events, event_listen_cnt, event_data, event_data->user_data);
        mthread_mutex_unlock(&mut_epoll);
        return -1;
    }
    else{
        event_listen_cnt++;
#ifdef EVENT_DEBUG
        LOG_INFO("[EVENT_ADD] ok fd=%d fd_type=%d events=0x%x listen_cnt=%d event_data=%p user_data=%p",
                 event_data->fd, event_data->fd_type, events, event_listen_cnt, event_data, event_data->user_data);
#endif
    }
    mthread_mutex_unlock(&mut_epoll);
    return 0;
}
int delEvent(event_data_ptr_t *event_data){
    if(event_data == NULL){
        LOG_ERROR("[EVENT_DEL] event_data is null listen_cnt=%d", event_listen_cnt);
        return -1;
    }
    mthread_mutex_lock(&mut_epoll);
    struct epoll_event epv = {0, {0}};
    epv.data.ptr = NULL;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, event_data->fd, &epv) < 0){
        LOG_ERROR("[EVENT_DEL] failed fd=%d fd_type=%d events=0x%x listen_cnt=%d event_data=%p user_data=%p",
                  event_data->fd, event_data->fd_type, event_data->events, event_listen_cnt, event_data, event_data->user_data);
        mthread_mutex_unlock(&mut_epoll);
        return -1;
    }
    else{
        event_listen_cnt--;
#ifdef EVENT_DEBUG
        LOG_WARN("[EVENT_DEL] ok fd=%d fd_type=%d events=0x%x listen_cnt=%d event_data=%p user_data=%p",
                 event_data->fd, event_data->fd_type, event_data->events, event_listen_cnt, event_data, event_data->user_data);
#endif
    }
    mthread_mutex_unlock(&mut_epoll);
    return 0;
}
void *startEventLoop(void *arg){
    while (run_flag == 1){
        struct epoll_event events[EPOLL_MAX];
        mthread_mutex_lock(&mut_epoll);
        int timeout = 10; // ms
        int nfd = epoll_wait(epoll_fd, events, EPOLL_MAX, timeout);
        mthread_mutex_unlock(&mut_epoll);
        if(nfd < 0){
            LOG_ERROR("[EPOLL_WAIT] failed epoll_fd=%d", epoll_fd);
            exit(-1);
        }
        if(nfd > 0){
            LOG_INFO("[EPOLL_BATCH] nfd=%d listen_cnt=%d", nfd, event_listen_cnt);
        }
        for(int i = 0; i < nfd; i++){
            event_data_ptr_t *event_data = (event_data_ptr_t *)events[i].data.ptr;
            int close_flag = 0;
            LOG_INFO("[EPOLL_EVENT_PTR] idx=%d/%d raw_events=0x%x event_data=%p",
                     i + 1, nfd, events[i].events, event_data);
            if(event_data == NULL){
                LOG_WARN("[EPOLL_EVENT] skip null event_data idx=%d/%d raw_events=0x%x",
                         i + 1, nfd, events[i].events);
                continue;
            }
            LOG_INFO("[EPOLL_EVENT] begin idx=%d/%d fd=%d fd_type=%d registered_events=0x%x raw_events=0x%x user_data=%p",
                     i + 1, nfd, event_data->fd, event_data->fd_type, event_data->events,
                     events[i].events, event_data->user_data);
            if((events[i].events & EPOLLIN)){
                if(event_callbacks.event_in){
                    LOG_INFO("[EPOLL_CALLBACK] event_in fd=%d fd_type=%d event_data=%p user_data=%p",
                             event_data->fd, event_data->fd_type, event_data, event_data->user_data);
                    if(event_callbacks.event_in(event_data) < 0){
                        close_flag = 1;
                        LOG_WARN("[EPOLL_CALLBACK] event_in request close fd=%d fd_type=%d event_data=%p",
                                 event_data->fd, event_data->fd_type, event_data);
                    }
                }
            }
            if((events[i].events & EPOLLERR) || (events[i].events & EPOLLRDHUP) || (events[i].events & EPOLLHUP)){
                close_flag = 1;
                LOG_WARN("[EPOLL_EVENT] close event fd=%d fd_type=%d raw_events=0x%x event_data=%p user_data=%p",
                         event_data->fd, event_data->fd_type, events[i].events, event_data, event_data->user_data);
            }
            else if ((events[i].events & EPOLLOUT)){
                if(event_callbacks.event_out){
                    LOG_INFO("[EPOLL_CALLBACK] event_out fd=%d fd_type=%d event_data=%p user_data=%p",
                             event_data->fd, event_data->fd_type, event_data, event_data->user_data);
                    if(event_callbacks.event_out(event_data) < 0){
                        close_flag = 1;
                        LOG_WARN("[EPOLL_CALLBACK] event_out request close fd=%d fd_type=%d event_data=%p",
                                 event_data->fd, event_data->fd_type, event_data);
                    }
                }
            }
            if(close_flag == 1){
                if(event_callbacks.event_close){
                    LOG_WARN("[EPOLL_CALLBACK] event_close begin fd=%d fd_type=%d event_data=%p user_data=%p",
                             event_data->fd, event_data->fd_type, event_data, event_data->user_data);
                    event_callbacks.event_close(event_data);
                    LOG_WARN("[EPOLL_CALLBACK] event_close end event_data=%p, pointer may have been freed",
                             event_data);
                }
            }
            LOG_INFO("[EPOLL_EVENT] end idx=%d/%d event_data=%p close_flag=%d", i + 1, nfd, event_data, close_flag);
        }
        m_sleep(10); // 10ms
    }
    return NULL;
}
void stopEventLoop(){
    run_flag = 0;
    return;
}
#endif
