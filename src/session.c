#include "session.h"

#include "rtcp_feedback.h"

#define SESSION_DEBUG
#define RTCP_SR_INTERVAL_MS 5000
static char mp4Dir[1024];
static int reloop_flag = 1;
static mthread_t event_thd;

static struct session_st *session_arr[FILEMAX]; // Session array, dynamically add and delete
static mthread_mutex_t mut_session;
/*
 * lock  mut_session(session_arr) --> lock session_arr[i].mutx(session_st) --> lock session_arr[i].clientinfo_st[j].mut_list(To read and write to the circular buffer queue of the clientinfo_st)
 *  --> unlock clientinfo_st.mut_list --> unlock session_st.mutx --> unlock mut_session
 * Not all operations require the addition of the above three locks, but when adding more locks, follow the locking sequence above to prevent deadlocks
 */

static int sum_client = 0; // Record how many clients are currently connecting to the server in total
static mthread_mutex_t mut_clientcount;
static uint32_t generateSsrc(socket_t seed_fd, const void *seed_ptr){
    static uint32_t ssrc_counter = 0x13572468;
    uintptr_t ptr_value;
    uint64_t time_seed;
    uint32_t ssrc = 0;

#if defined(__linux__) || defined(__linux)
    FILE *fp = fopen("/dev/urandom", "rb");
    if(fp != NULL){
        if(fread(&ssrc, sizeof(ssrc), 1, fp) != 1){
            ssrc = 0;
        }
        fclose(fp);
    }
#endif

    ptr_value = (uintptr_t)seed_ptr;
    time_seed = getTimeMs();
    if(ssrc == 0){
        ssrc = (uint32_t)(time_seed ^ (time_seed >> 32));
    }
    ssrc ^= (uint32_t)seed_fd;
    ssrc ^= (uint32_t)ptr_value;
    ssrc ^= (uint32_t)(ptr_value >> 32);
    ssrc ^= ssrc_counter;
    ssrc_counter += 0x9E3779B9U;

    /* Final xorshift mix so nearby seeds still spread across the 32-bit space. */
    ssrc ^= ssrc << 13;
    ssrc ^= ssrc >> 17;
    ssrc ^= ssrc << 5;
    if(ssrc == 0){
        ssrc = 0x01020304U ^ ssrc_counter;
    }
    return ssrc;
}
static int sendRtcpSenderReport(struct clientinfo_st *clientinfo, struct RtcpSenderContext *rtcp_ctx, struct RtpPacket *rtp_packet, socket_t rtcp_fd, int rtcp_channel, int client_rtcp_port){
    uint8_t rtcp_packet_buffer[28];
    struct rtp_tcp_header tcp_header;
    uint32_t value32;
    int ret;

    if(clientinfo == NULL || rtcp_ctx == NULL || rtp_packet == NULL){
        return -1;
    }
    if(rtcp_ctx->last_ntp_timestamp == 0){
        return 0;
    }

    memset(rtcp_packet_buffer, 0, sizeof(rtcp_packet_buffer));
    rtcp_packet_buffer[0] = (RTP_VESION << 6);
    rtcp_packet_buffer[1] = 200;
    rtcp_packet_buffer[2] = 0;
    rtcp_packet_buffer[3] = 6;

    value32 = htonl(rtp_packet->rtpHeader.ssrc);
    memcpy(rtcp_packet_buffer + 4, &value32, sizeof(value32));
    value32 = htonl((uint32_t)(rtcp_ctx->last_ntp_timestamp >> 32));
    memcpy(rtcp_packet_buffer + 8, &value32, sizeof(value32));
    value32 = htonl((uint32_t)(rtcp_ctx->last_ntp_timestamp & 0xffffffffULL));
    memcpy(rtcp_packet_buffer + 12, &value32, sizeof(value32));
    value32 = htonl(rtcp_ctx->last_rtp_timestamp);
    memcpy(rtcp_packet_buffer + 16, &value32, sizeof(value32));
    value32 = htonl(rtcp_ctx->packet_count);
    memcpy(rtcp_packet_buffer + 20, &value32, sizeof(value32));
    value32 = htonl(rtcp_ctx->octet_count);
    memcpy(rtcp_packet_buffer + 24, &value32, sizeof(value32));

    if(clientinfo->transport == RTP_OVER_TCP){
        if(clientinfo->sd == INVALID_SOCKET || rtcp_channel < 0){
            return -1;
        }
        tcp_header.magic = '$';
        tcp_header.channel = (uint8_t)rtcp_channel;
        tcp_header.rtp_len16 = htons((uint16_t)sizeof(rtcp_packet_buffer));
        ret = sendWithTimeout(clientinfo->sd, (const char *)&tcp_header, sizeof(tcp_header), 0);
        if(ret <= 0){
            return -1;
        }
        ret = sendWithTimeout(clientinfo->sd, (const char *)rtcp_packet_buffer, sizeof(rtcp_packet_buffer), 0);
        if(ret <= 0){
            return -1;
        }
        return ret;
    }
    if(rtcp_fd == INVALID_SOCKET || client_rtcp_port < 0){
        return -1;
    }
    return sendUDP(rtcp_fd, (const char *)rtcp_packet_buffer, sizeof(rtcp_packet_buffer), clientinfo->client_ip, client_rtcp_port, 0);
}
static int maybeSendRtcpSenderReport(struct clientinfo_st *clientinfo, int media_type, struct RtpPacket *rtp_packet, struct RtcpPacketInfo *rtcp_info){
    struct RtcpSenderContext *rtcp_ctx;
    socket_t rtcp_fd;
    int rtcp_channel;
    int client_rtcp_port;

    if(clientinfo == NULL || rtp_packet == NULL || rtcp_info == NULL){
        return -1;
    }

    if(media_type == VIDEO){
        rtcp_ctx = &clientinfo->rtcp_video;
        rtcp_fd = clientinfo->udp_sd_rtcp;
        rtcp_channel = clientinfo->sig_1;
        client_rtcp_port = clientinfo->client_rtcp_port;
    }
    else{
        rtcp_ctx = &clientinfo->rtcp_audio;
        rtcp_fd = clientinfo->udp_sd_rtcp_1;
        rtcp_channel = clientinfo->sig_3;
        client_rtcp_port = clientinfo->client_rtcp_port_1;
    }

    rtcp_ctx->packet_count += rtcp_info->packet_count;
    rtcp_ctx->octet_count += rtcp_info->octet_count;
    rtcp_ctx->last_rtp_timestamp = rtcp_info->rtp_timestamp;
    rtcp_ctx->last_ntp_timestamp = rtcp_info->ntp_timestamp;

    if(rtcp_ctx->last_sr_time_ms != 0 && (rtcp_info->wallclock_ms - rtcp_ctx->last_sr_time_ms) < RTCP_SR_INTERVAL_MS){
        return 0;
    }
    if(sendRtcpSenderReport(clientinfo, rtcp_ctx, rtp_packet, rtcp_fd, rtcp_channel, client_rtcp_port) < 0){
        return -1;
    }
    rtcp_ctx->last_sr_time_ms = rtcp_info->wallclock_ms;
    return 0;
}
static int eventAdd(int events, struct clientinfo_st *ev){
    if (ev->sd == INVALID_SOCKET)
        return -1;
    // media
    if(ev->transport == RTP_OVER_TCP){
        event_data_ptr_t *event_data = (event_data_ptr_t *)malloc(sizeof(event_data_ptr_t));
        ev->event_data[0] = event_data;
        event_data->user_data = (void *)ev;
        event_data->fd = ev->sd;
        event_data->fd_type = FD_TYPE_TCP;
        ev->events = events | EVENT_IN; // client heartbeat(rtsp)
        if(addEvent(ev->events, event_data) < 0){
            return -1;
        }
    }
    else{
        // Monitor client TCP connections and handle client shutdown events
        event_data_ptr_t *event_data = (event_data_ptr_t *)malloc(sizeof(event_data_ptr_t));
        ev->event_data[0] = event_data;
        event_data->user_data = (void *)ev;
        event_data->fd = ev->sd;
        event_data->fd_type = FD_TYPE_TCP;
        ev->events = events | EVENT_IN; // client heartbeat(rtsp)
        if(addEvent(ev->events, event_data) < 0){
            return -1;
        }

        if (ev->udp_sd_rtp != INVALID_SOCKET){ // video
            event_data_ptr_t *event_data = (event_data_ptr_t *)malloc(sizeof(event_data_ptr_t));
            ev->event_data[1] = event_data;
            event_data->user_data = (void *)ev;
            event_data->fd = ev->udp_sd_rtp;
            event_data->fd_type = FD_TYPE_UDP_RTP;
            ev->events = events;
            if(addEvent(ev->events, event_data) < 0){
                return -1;
            }
        }
        if (ev->udp_sd_rtcp != INVALID_SOCKET){ // video rtcp
            event_data_ptr_t *event_data = (event_data_ptr_t *)malloc(sizeof(event_data_ptr_t));
            ev->event_data[2] = event_data;
            event_data->user_data = (void *)ev;
            event_data->fd = ev->udp_sd_rtcp;
            event_data->fd_type = FD_TYPE_UDP_RTCP;
            if(addEvent(EVENT_IN, event_data) < 0){
                return -1;
            }
        }
        if(ev->udp_sd_rtp_1 != INVALID_SOCKET){ // audio
            event_data_ptr_t *event_data = (event_data_ptr_t *)malloc(sizeof(event_data_ptr_t));
            ev->event_data[3] = event_data;
            event_data->user_data = (void *)ev;
            event_data->fd = ev->udp_sd_rtp_1;
            event_data->fd_type = FD_TYPE_UDP_RTP;
            ev->events = events;
            if(addEvent(ev->events, event_data) < 0){
                return -1;
            }
        }
        if(ev->udp_sd_rtcp_1 != INVALID_SOCKET){ // audio rtcp
            event_data_ptr_t *event_data = (event_data_ptr_t *)malloc(sizeof(event_data_ptr_t));
            ev->event_data[4] = event_data;
            event_data->user_data = (void *)ev;
            event_data->fd = ev->udp_sd_rtcp_1;
            event_data->fd_type = FD_TYPE_UDP_RTCP;
            if(addEvent(EVENT_IN, event_data) < 0){
                return -1;
            }
        }
    }
    return 0;
}
// handle client heartbeat(rtsp) or TEARDOWN or RTCP(Just care about TCP's RTCP and UDP's direct dropout)
// TCP data must be processed, otherwise it will block the other end. UDP does not have this problem
/**
 * RTP-over-TCP 模式下，客户端会把控制/RTCP走同一条 TCP。
 * 不读，TCP 接收缓冲会堆满，最终两端阻塞。UDP 没这个“流式背压阻塞”问题，丢包就丢，不会把连接卡死。
 */
static int charEqIgnoreCase(char a, char b){
    if(a >= 'A' && a <= 'Z'){
        a = (char)(a - 'A' + 'a');
    }
    if(b >= 'A' && b <= 'Z'){
        b = (char)(b - 'A' + 'a');
    }
    return a == b;
}
static int startsWithIgnoreCase(const char *s, int s_len, const char *prefix){
    int i = 0;
    int prefix_len = (int)strlen(prefix);
    if(s == NULL || prefix == NULL || s_len < prefix_len){
        return 0;
    }
    for(i = 0; i < prefix_len; i++){
        if(!charEqIgnoreCase(s[i], prefix[i])){
            return 0;
        }
    }
    return 1;
}

/**
 * @description: header 结束检测，返回 header 结束位置（即 body 开始位置），如果没有找到 header 结束标志，则返回 -1
 * @param {char} *buffer
 * @param {int} len
 * @return {*}
 */
static int findRtspHeaderEnd(const char *buffer, int len){
    int i;
    if(buffer == NULL || len <= 0){
        return -1;
    }
    for(i = 0; i + 3 < len; i++){
        if(buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n'){
            return i + 4;
        }
    }
    for(i = 0; i + 1 < len; i++){
        if(buffer[i] == '\n' && buffer[i + 1] == '\n'){
            return i + 2;
        }
    }
    return -1;
}

/**
 * @description: 获取 RTSP 消息的 Content-Length,确保 body 收全再处理
 * @param {char} *buffer
 * @param {int} header_len
 * @return {*}
 */
static int getRtspContentLength(const char *buffer, int header_len){
    int i = 0;
    if(buffer == NULL || header_len <= 0){
        return 0;
    }
    while(i < header_len){
        int line_start = i;
        int line_end = i;
        while(line_end < header_len && buffer[line_end] != '\n'){
            line_end++;
        }
        if(line_end > line_start){
            int line_len = line_end - line_start;
            if(line_len > 0 && buffer[line_end - 1] == '\r'){
                line_len--;
            }
            if(startsWithIgnoreCase(buffer + line_start, line_len, "Content-Length")){
                int colon = line_start;
                while(colon < line_start + line_len && buffer[colon] != ':'){
                    colon++;
                }
                if(colon < line_start + line_len && buffer[colon] == ':'){
                    int value = 0;
                    int pos = colon + 1;
                    while(pos < line_start + line_len && (buffer[pos] == ' ' || buffer[pos] == '\t')){
                        pos++;
                    }
                    while(pos < line_start + line_len && buffer[pos] >= '0' && buffer[pos] <= '9'){
                        value = value * 10 + (buffer[pos] - '0');
                        pos++;
                    }
                    return value;
                }
            }
        }
        i = (line_end < header_len) ? (line_end + 1) : header_len;
    }
    return 0;
}
static int handleCmd_400(char *result, int cseq){
    snprintf(result, 4096, "RTSP/1.0 400 Bad Request\r\n"
                           "CSeq: %d\r\n"
                           "\r\n", cseq);
    return 0;
}
static int handleCmd_405(char *result, int cseq){
    snprintf(result, 4096, "RTSP/1.0 405 Method Not Allowed\r\n"
                           "CSeq: %d\r\n"
                           "Allow: OPTIONS, GET_PARAMETER, SET_PARAMETER, PLAY, PAUSE, TEARDOWN\r\n"
                           "\r\n", cseq);
    return 0;
}

/**
 * @description: 分发 RTSP 控制命令
 * @param {char} *result
 * @param {rtsp_request_message_st} *request_message
 * @param {int} cseq
 * @param {char} *session
 * @param {int} *need_close
 * @return {*}
 */
static int dispatchRtspControl(char *result, struct rtsp_request_message_st *request_message, int cseq, char *session, int *need_close){
    if(result == NULL || request_message == NULL || need_close == NULL){
        return -1;
    }
    *need_close = 0;
    if(strcmp(request_message->method, "OPTIONS") == 0){
        return handleCmd_OPTIONS(result, cseq);
    }
    if(strcmp(request_message->method, "GET_PARAMETER") == 0 || strcmp(request_message->method, "SET_PARAMETER") == 0 || strcmp(request_message->method, "PLAY") == 0 || strcmp(request_message->method, "PAUSE") == 0){
        return handleCmd_General(result, cseq, session);
    }
    if(strcmp(request_message->method, "TEARDOWN") == 0){
        *need_close = 1;
        return handleCmd_General(result, cseq, session);
    }
    return handleCmd_405(result, cseq);
}

/**
 * @description: 统一处理客户端 IO 可读事件
 * 1) TCP socket: RTSP 控制消息 + RTP/RTCP over TCP($ 交织帧)
 * 2) UDP RTCP socket: 接收并解析 RTCP 反馈（质量统计）
 * @param {event_data_ptr_t} *event_data
 * @return {*}
 */
static int handleClientIoData(event_data_ptr_t *event_data){
    struct clientinfo_st *clientinfo = (struct clientinfo_st *)event_data->user_data;
    int type = event_data->fd_type;
    socket_t fd = event_data->fd;
    int recv_len;
    int capacity;
    char buffer_send[4096];

    if(clientinfo == NULL){
        return -1;
    }
    // UDP RTCP 反馈通道：读取一个 RTCP datagram 并更新统计
    if(type == FD_TYPE_UDP_RTCP && (fd == clientinfo->udp_sd_rtcp || fd == clientinfo->udp_sd_rtcp_1)){
        char udp_buf[1600];
        char peer_ip[64] = {0};
        int peer_port = 0;
        recv_len = recvUDP(fd, udp_buf, sizeof(udp_buf), peer_ip, &peer_port, 0);
        if(recv_len > 0){
            if(fd == clientinfo->udp_sd_rtcp){
                // 视频 RTCP
                rtcpHandleUdp(clientinfo, 0, (const uint8_t *)udp_buf, recv_len);
            }
            else{
                // 音频 RTCP
                rtcpHandleUdp(clientinfo, 1, (const uint8_t *)udp_buf, recv_len);
            }
        }
        return 0;
    }
    if(fd != clientinfo->sd){
        return 0;
    }

    if(clientinfo->len < 0 || clientinfo->len >= (int)sizeof(clientinfo->buffer)){
        return -1;
    }
    capacity = (int)sizeof(clientinfo->buffer) - clientinfo->pos - 1;
    if(capacity <= 0){
        return -1;
    }
    recv_len = recvWithTimeout(clientinfo->sd, clientinfo->buffer + clientinfo->pos, capacity, 0);
    if(recv_len <= 0){
        return -1;
    }
    clientinfo->len += recv_len;
    clientinfo->pos = clientinfo->len;
    clientinfo->buffer[clientinfo->len] = 0;

    while(clientinfo->len > 0){
        if(clientinfo->buffer[0] == '$'){ // interleaved RTP/RTCP over TCP
            int rtcp_len;
            int frame_len;
            uint8_t channel;
            if(clientinfo->len < 4){
                break;
            }
            // $ + channel + length(2B) + payload
            channel = (uint8_t)clientinfo->buffer[1];
            rtcp_len = ((unsigned char)clientinfo->buffer[2] << 8) | (unsigned char)clientinfo->buffer[3];
            frame_len = rtcp_len + 4;
            if(clientinfo->len < frame_len){
                break;
            }
            // 交织通道里的 RTCP 反馈统计
            rtcpHandleInterleaved(clientinfo, channel, (const uint8_t *)(clientinfo->buffer + 4), rtcp_len);
            clientinfo->len -= frame_len;
            if(clientinfo->len > 0){
                memmove(clientinfo->buffer, clientinfo->buffer + frame_len, clientinfo->len);
            }
            clientinfo->pos = clientinfo->len;
            clientinfo->buffer[clientinfo->len] = 0;
            continue;
        }
        else{
            struct rtsp_request_message_st request_message;
            int header_len = findRtspHeaderEnd(clientinfo->buffer, clientinfo->len);
            int body_len;
            int total_msg_len;
            int parse_used;
            int need_close = 0;
            char *CSeq;
            char *Session;
            int cseq;

            if(header_len < 0){
                break;
            }
            // 支持带 body 的 RTSP 控制请求（如 SET_PARAMETER）
            body_len = getRtspContentLength(clientinfo->buffer, header_len);
            total_msg_len = header_len + body_len;
            if(total_msg_len > clientinfo->len){
                break;
            }

            memset(&request_message, 0, sizeof(struct rtsp_request_message_st));
            parse_used = parseRtspRequest(clientinfo->buffer, total_msg_len, &request_message);
            if(parse_used < 0){
                return -1;
            }

            CSeq = findValueByKey(&request_message, "CSeq");
            cseq = (CSeq != NULL) ? atoi(CSeq) : 0;
            if(CSeq == NULL){
                handleCmd_400(buffer_send, cseq);
            }
            else{
                Session = findValueByKey(&request_message, "Session");
                if(dispatchRtspControl(buffer_send, &request_message, cseq, Session, &need_close) < 0){
                    return -1;
                }
            }

            if(sendWithTimeout(clientinfo->sd, (const char*)buffer_send, (int)strlen(buffer_send), 0) <= 0){
                return -1;
            }

            clientinfo->len -= total_msg_len;
            if(clientinfo->len > 0){
                memmove(clientinfo->buffer, clientinfo->buffer + total_msg_len, clientinfo->len);
            }
            clientinfo->pos = clientinfo->len;
            clientinfo->buffer[clientinfo->len] = 0;

            if(need_close){
                return -1;
            }
        }
    }
    return 0;
}
static int sendClientMedia(event_data_ptr_t *event_data){
    struct clientinfo_st *clientinfo = (struct clientinfo_st *)event_data->user_data;
    int type = event_data->fd_type;
    socket_t fd = event_data->fd;
    if(clientinfo == NULL){
        return -1;
    }
    // Retrieve data from the circular queue and send it
    mthread_mutex_lock(&clientinfo->session->mut); // Reading the client info in the session, locking is required
    mthread_mutex_lock(&clientinfo->mut_list);
    struct MediaPacket_st node;
    node.size = 0;
    if(fd == clientinfo->sd && ((clientinfo->sig_0 != -1) || (clientinfo->sig_2 != -1)) && (type == FD_TYPE_TCP)){ // rtp over tcp
        // Extract a frame of audio or video
        node = getFrameFromList1(clientinfo);
    }
    else if(fd == clientinfo->udp_sd_rtp && type == FD_TYPE_UDP_RTP){ // video
        // Extract a frame of video
        node = getFrameFromList1(clientinfo);
    }
    else if(fd == clientinfo->udp_sd_rtp_1 && type == FD_TYPE_UDP_RTP){ // audio
        // Extract a frame of audio
        node = getFrameFromList2(clientinfo);
    }
    mthread_mutex_unlock(&clientinfo->mut_list);
    mthread_mutex_unlock(&clientinfo->session->mut);
    if (node.size == 0){ // No data to send
        return 0;
    }
    enum VIDEO_e video_type = getSessionVideoType(clientinfo->session);
    int sample_rate;
    int channels;
    int profile;
    int ret;
    struct RtcpPacketInfo rtcp_info;
    ret = getSessionAudioInfo(clientinfo->session, &sample_rate, &channels, &profile);
    enum AUDIO_e audio_type = getSessionAudioType(clientinfo->session);
    if (fd == clientinfo->sd){ // rtp over tcp
        if(node.type == VIDEO){
            memset(&rtcp_info, 0, sizeof(rtcp_info));
            switch(video_type){
                case VIDEO_H264:
                    ret = rtpSendH264Frame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet, node.data, node.size, 0, clientinfo->sig_0, NULL, -1, &rtcp_info);
                    break;
                case VIDEO_H265:
                    ret = rtpSendH265Frame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet, node.data, node.size, 0, clientinfo->sig_0, NULL, -1, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
        else{
            memset(&rtcp_info, 0, sizeof(rtcp_info));
            switch(audio_type){
                case AUDIO_AAC:
                    ret = rtpSendAACFrame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet_1, node.data, node.size, sample_rate, channels, profile, clientinfo->sig_2, NULL, -1, &rtcp_info);
                    break;
                case AUDIO_PCMA:
                    ret = rtpSendPCMAFrame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet_1, node.data, node.size, sample_rate, channels, profile, clientinfo->sig_2, NULL, -1, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
    }
    else{ // rtp over udp
        if(node.type == VIDEO){
            memset(&rtcp_info, 0, sizeof(rtcp_info));
            switch(video_type){
                case VIDEO_H264:
                    ret = rtpSendH264Frame(clientinfo->udp_sd_rtp, NULL, clientinfo->rtp_packet, node.data, node.size, 0, -1, clientinfo->client_ip, clientinfo->client_rtp_port, &rtcp_info);
                    break;
                case VIDEO_H265:
                    ret = rtpSendH265Frame(clientinfo->udp_sd_rtp, NULL, clientinfo->rtp_packet, node.data, node.size, 0, -1, clientinfo->client_ip, clientinfo->client_rtp_port, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
        else{
            memset(&rtcp_info, 0, sizeof(rtcp_info));
            switch(audio_type){
                case AUDIO_AAC:
                    ret = rtpSendAACFrame(clientinfo->udp_sd_rtp_1, NULL, clientinfo->rtp_packet_1, node.data, node.size, sample_rate, channels, profile, -1, clientinfo->client_ip, clientinfo->client_rtp_port_1, &rtcp_info);
                    break;
                case AUDIO_PCMA:
                    ret = rtpSendPCMAFrame(clientinfo->udp_sd_rtp_1, NULL, clientinfo->rtp_packet_1, node.data, node.size, sample_rate, channels, profile, -1, clientinfo->client_ip, clientinfo->client_rtp_port_1, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
    }
    if(ret <= 0){
        return -1;
    }
    if(maybeSendRtcpSenderReport(clientinfo, node.type, node.type == VIDEO ? clientinfo->rtp_packet : clientinfo->rtp_packet_1, &rtcp_info) < 0){
        return -1;
    }
    return 0;
}
static int eventDel(struct clientinfo_st *ev)
{
    if(ev->sd == INVALID_SOCKET)
        return -1;
    for(int i = 0; i < sizeof(ev->event_data) / sizeof(ev->event_data[0]); i++){
        if(ev->event_data[i] != NULL){
            if(delEvent(ev->event_data[i]) < 0){
                return -1;
            }
        }
    }
    return 0;
}
static int delClient(event_data_ptr_t *event_data){
    struct clientinfo_st *clientinfo = (struct clientinfo_st *)event_data->user_data;
    int type = event_data->fd_type;
    socket_t fd = event_data->fd;
    if(clientinfo == NULL || clientinfo->sd == INVALID_SOCKET){
        return -1;
    }
    if(eventDel(clientinfo) < 0){ // Delete all listening sockets(TCP/UDP)
        return -1;
    }
#ifdef SESSION_DEBUG
    printf("client:%d offline\n", clientinfo->sd);
#endif
    struct session_st *session = clientinfo->session;
    int count = 0;
    mthread_mutex_lock(&clientinfo->session->mut);
    clearClient(clientinfo);
    session->count--;
    count = session->count;
    mthread_mutex_unlock(&clientinfo->session->mut);
    /*Change the total number of customer connections*/
    mthread_mutex_lock(&mut_clientcount);
    sum_client--;
#ifdef SESSION_DEBUG
    printf("sum_client:%d\n", sum_client);
#endif
    mthread_mutex_unlock(&mut_clientcount);
    if(count == 0){
        if(clientinfo->session->is_custom == 0){
#ifdef RTSP_FILE_SERVER
            delFileSession(session_arr[clientinfo->session->pos]);
#endif
        }
    }
    return 0;
}
#ifdef RTSP_FILE_SERVER
/*Audio and video queue operation*/
static void sendData(void *arg)
{
    struct clientinfo_st *clientinfo = (struct clientinfo_st *)arg;
    mthread_mutex_lock(&clientinfo->mut_list);
    if(clientinfo->packet_num >= RING_BUFFER_MAX){
        printf("WARING ring buffer too large\n");
    }
    // The data packet is sent to the circular queue
    if(nowStreamIsVideo(clientinfo->session->media) && (clientinfo->sig_0 != -1 || clientinfo->client_rtp_port != -1)){ // video
        char *ptr = NULL;
        int ptr_len = 0;
        getVideoNALUWithoutStartCode(clientinfo->session->media, &ptr, &ptr_len);
        pushFrameToList1(clientinfo, ptr, ptr_len, VIDEO);
    }
    if(nowStreamIsAudio(clientinfo->session->media) && (clientinfo->sig_2 != -1 || clientinfo->client_rtp_port_1 != -1)){ // audio
        char *ptr = NULL;
        int ptr_len = 0;
        getAudioWithoutADTS(clientinfo->session->media, &ptr, &ptr_len);
        if(clientinfo->client_rtp_port_1 != -1){ // UDP, Use different queues for audio and video
            pushFrameToList2(clientinfo, ptr, ptr_len, AUDIO);
        }
        else if(clientinfo->sig_2 != -1){ // TCP, audio and video use the same queue
            pushFrameToList1(clientinfo, ptr, ptr_len, AUDIO);
        }
    }
    mthread_mutex_unlock(&clientinfo->mut_list);
    return;
}
#endif
static int get_free_clientinfo(int pos)
{
    for(int i = 0; i < CLIENTMAX; i++){
        if(session_arr[pos]->clientinfo[i].sd == INVALID_SOCKET){
            return i;
        }
    }
    return -1;
}
int configSession(int file_reloop_flag, const char *mp4_file_path) {
    reloop_flag = file_reloop_flag;
    memset(mp4Dir, 0, sizeof(mp4Dir));
    if(mp4_file_path != NULL) {
        if(mp4_file_path[strlen(mp4_file_path) - 1] == '/') {
            strncpy(mp4Dir, mp4_file_path, sizeof(mp4Dir) - 1);
        } 
        else{
            size_t len = strlen(mp4_file_path);
            strncpy(mp4Dir, mp4_file_path, sizeof(mp4Dir) - 2);
            mp4Dir[len] = '/';
        }
    }
    else{
        memcpy(mp4Dir,"./mp4path/", strlen("./mp4path/"));
    }
#ifdef SESSION_DEBUG
    printf("reloop_flag:%d mp4Dir:%s\n", reloop_flag, mp4Dir);
#endif
    return 0;
}
void sig_handler(int s)
{
    printf("catch signal %d,rtsp exit\n", s);
    moduleDel();
    printf("%s\n", __func__);
    exit(1);
}
int moduleInit()
{
    socketInit();
    memset(session_arr, 0, sizeof(struct session_st *));
    if(createEvent() < 0){
        return -1;
    }
    /* 设置事件回调函数 */
    setEventCallback(handleClientIoData, sendClientMedia, delClient);

    /* 创建事件循环线程，此时还未添加任何事件 */
    int ret = mthread_create(&event_thd, NULL, startEventLoop, NULL);
    if(ret < 0){
        printf("startEventLoop mthread_create()\n");
        return -1;
    }
    mthread_detach(event_thd);
    mthread_mutex_init(&mut_session, NULL);
    mthread_mutex_init(&mut_clientcount, NULL);
    return 0;
}

void moduleDel()
{
#ifdef RTSP_FILE_SERVER
    for(int i = 0; i < FILEMAX; i++){
        delFileSession(session_arr[i]);
    }
#endif
    stopEventLoop();
    closeEvent();
    mthread_mutex_destroy(&mut_session);
    mthread_mutex_destroy(&mut_clientcount);
    socketDestroy();
    return;
}
int initClient(struct session_st *session, struct clientinfo_st *clientinfo)
{
    if(session == NULL || clientinfo == NULL){
        return -1;
    }
    clientinfo->sd = INVALID_SOCKET;
    clientinfo->udp_sd_rtp = INVALID_SOCKET;
    clientinfo->udp_sd_rtcp = INVALID_SOCKET;
    clientinfo->udp_sd_rtp_1 = INVALID_SOCKET;
    clientinfo->udp_sd_rtcp_1 = INVALID_SOCKET;
    for(int i = 0; i < sizeof(clientinfo->event_data) / sizeof(clientinfo->event_data[0]); i++){
        clientinfo->event_data[i] = NULL;
    }
    memset(clientinfo->client_ip, 0, sizeof(clientinfo->client_ip));
    clientinfo->client_rtp_port = -1;
    clientinfo->client_rtcp_port = -1;
    clientinfo->client_rtp_port_1 = -1;
    clientinfo->client_rtcp_port_1 = -1;
    clientinfo->transport = -1;
    clientinfo->sig_0 = -1;
    clientinfo->sig_1 = -1;
    clientinfo->sig_2 = -1;
    clientinfo->sig_3 = -1;
    clientinfo->playflag = -1;

    clientinfo->send_call_back = NULL;
    clientinfo->events = -1;
    clientinfo->session = session;

    clientinfo->rtp_packet = NULL;
    clientinfo->rtp_packet_1 = NULL;
    clientinfo->tcp_header = NULL;
    memset(&clientinfo->rtcp_video, 0, sizeof(clientinfo->rtcp_video));
    memset(&clientinfo->rtcp_audio, 0, sizeof(clientinfo->rtcp_audio));
    memset(&clientinfo->rtcp_rx_video, 0, sizeof(clientinfo->rtcp_rx_video));
    memset(&clientinfo->rtcp_rx_audio, 0, sizeof(clientinfo->rtcp_rx_audio));

    // video
    mthread_mutex_init(&clientinfo->mut_list, NULL);
    clientinfo->packet_list = NULL;
    clientinfo->packet_list_size = 0;
    clientinfo->pos_list = 0;
    clientinfo->packet_num = 0;
    clientinfo->pos_last_packet = 0;

    // audio
    clientinfo->packet_list_1 = NULL;
    clientinfo->packet_list_size_1 = 0;
    clientinfo->pos_list_1 = 0;
    clientinfo->packet_num_1 = 0;
    clientinfo->pos_last_packet_1 = 0;

    memset(clientinfo->buffer, 0, sizeof(clientinfo->buffer));
    clientinfo->len = 0;
    clientinfo->pos = 0;
    return 0;
}
int clearClient(struct clientinfo_st *clientinfo)
{
    if(clientinfo == NULL){
        return -1;
    }
    if(clientinfo->sd != INVALID_SOCKET){
        closeSocket(clientinfo->sd);
        clientinfo->sd = INVALID_SOCKET;
    }
    if(clientinfo->udp_sd_rtp != INVALID_SOCKET){
        closeSocket(clientinfo->udp_sd_rtp);
        clientinfo->udp_sd_rtp = INVALID_SOCKET;
    }
    if(clientinfo->udp_sd_rtcp != INVALID_SOCKET){
        closeSocket(clientinfo->udp_sd_rtcp);
        clientinfo->udp_sd_rtcp = INVALID_SOCKET;
    }
    if(clientinfo->udp_sd_rtp_1 != INVALID_SOCKET){
        closeSocket(clientinfo->udp_sd_rtp_1);
        clientinfo->udp_sd_rtp_1 = INVALID_SOCKET;
    }
    if(clientinfo->udp_sd_rtcp_1 != INVALID_SOCKET){
        closeSocket(clientinfo->udp_sd_rtcp_1);
        clientinfo->udp_sd_rtcp_1 = INVALID_SOCKET;
    }
    for(int i = 0; i < sizeof(clientinfo->event_data) / sizeof(clientinfo->event_data[0]); i++){
        if(clientinfo->event_data[i] != NULL){
            free(clientinfo->event_data[i]);
            clientinfo->event_data[i] = NULL;
        }
    }
    memset(clientinfo->client_ip, 0, sizeof(clientinfo->client_ip));
    clientinfo->client_rtp_port = -1;
    clientinfo->client_rtcp_port = -1;
    clientinfo->client_rtp_port_1 = -1;
    clientinfo->client_rtcp_port_1 = -1;
    clientinfo->transport = -1;
    clientinfo->sig_0 = -1;
    clientinfo->sig_1 = -1;
    clientinfo->sig_2 = -1;
    clientinfo->sig_3 = -1;
    clientinfo->playflag = -1;

    clientinfo->send_call_back = NULL;
    clientinfo->events = -1;
    // clientinfo->session=NULL;

    if(clientinfo->rtp_packet != NULL){
        free(clientinfo->rtp_packet);
        clientinfo->rtp_packet = NULL;
    }
    if(clientinfo->rtp_packet_1 != NULL){
        free(clientinfo->rtp_packet_1);
        clientinfo->rtp_packet_1 = NULL;
    }
    if(clientinfo->tcp_header != NULL){
        free(clientinfo->tcp_header);
        clientinfo->tcp_header = NULL;
    }
    memset(&clientinfo->rtcp_video, 0, sizeof(clientinfo->rtcp_video));
    memset(&clientinfo->rtcp_audio, 0, sizeof(clientinfo->rtcp_audio));
    memset(&clientinfo->rtcp_rx_video, 0, sizeof(clientinfo->rtcp_rx_video));
    memset(&clientinfo->rtcp_rx_audio, 0, sizeof(clientinfo->rtcp_rx_audio));

    // video
    mthread_mutex_destroy(&clientinfo->mut_list);
    if(clientinfo->packet_list != NULL){
        free(clientinfo->packet_list);
        clientinfo->packet_list = NULL;
    }
    clientinfo->packet_list_size = 0;
    clientinfo->pos_list = 0;
    clientinfo->packet_num = 0;
    clientinfo->pos_last_packet = 0;

    // audio
    if(clientinfo->packet_list_1 != NULL){
        free(clientinfo->packet_list_1);
        clientinfo->packet_list_1 = NULL;
    }
    clientinfo->packet_list_size_1 = 0;
    clientinfo->pos_list_1 = 0;
    clientinfo->packet_num_1 = 0;
    clientinfo->pos_last_packet_1 = 0;

    memset(clientinfo->buffer, 0, sizeof(clientinfo->buffer));
    clientinfo->len = 0;
    clientinfo->pos = 0;

    return 0;
}
static int increaseClientList(enum MEDIA_e type, struct clientinfo_st *clientinfo)
{
    // |packet5|packet6|packet7|packet8|packet1(pos)|packet2|packet3|packet4| --> |packet5|packet6|packet7|packet8|space1|space2|space3|space4|packet1(pos)|packet2|packet3|packet4|
    if(type == VIDEO){
        if (clientinfo->packet_num >= clientinfo->packet_list_size){ // The buffer is used up, increase the buffer
            clientinfo->packet_list = (struct MediaPacket_st *)realloc(clientinfo->packet_list, (clientinfo->packet_list_size + 4) * sizeof(struct MediaPacket_st));
            memmove(clientinfo->packet_list + clientinfo->pos_list + 4, clientinfo->packet_list + clientinfo->pos_list, (clientinfo->packet_list_size - clientinfo->pos_list) * sizeof(struct MediaPacket_st));
            clientinfo->packet_list_size += 4;
            clientinfo->pos_list += 4;
        }
    }
    else if(type == AUDIO){
        if(clientinfo->packet_num_1 >= clientinfo->packet_list_size_1){ // The buffer is used up, increase the buffer
            clientinfo->packet_list_1 = (struct MediaPacket_st *)realloc(clientinfo->packet_list_1, (clientinfo->packet_list_size_1 + 4) * sizeof(struct MediaPacket_st));
            memmove(clientinfo->packet_list_1 + clientinfo->pos_list_1 + 4, clientinfo->packet_list_1 + clientinfo->pos_list_1, (clientinfo->packet_list_size_1 - clientinfo->pos_list_1) * sizeof(struct MediaPacket_st));
            clientinfo->packet_list_size_1 += 4;
            clientinfo->pos_list_1 += 4;
        }
    }
    return 0;
}
void pushFrameToList1(struct clientinfo_st *clientinfo, char *ptr, int ptr_len, int type){
    increaseClientList(VIDEO, clientinfo);
    memcpy(clientinfo->packet_list[clientinfo->pos_last_packet].data, ptr, ptr_len);
    clientinfo->packet_list[clientinfo->pos_last_packet].size = ptr_len;
    clientinfo->packet_list[clientinfo->pos_last_packet].type = type;
    clientinfo->packet_num++;
    clientinfo->pos_last_packet++;
    if(clientinfo->pos_last_packet >= clientinfo->packet_list_size){
        clientinfo->pos_last_packet = 0;
    }
    return;
}
void pushFrameToList2(struct clientinfo_st *clientinfo, char *ptr, int ptr_len, int type){
    increaseClientList(AUDIO, clientinfo);
    memcpy(clientinfo->packet_list_1[clientinfo->pos_last_packet_1].data, ptr, ptr_len);
    clientinfo->packet_list_1[clientinfo->pos_last_packet_1].size = ptr_len;
    clientinfo->packet_list_1[clientinfo->pos_last_packet_1].type = type;
    clientinfo->packet_num_1++;
    clientinfo->pos_last_packet_1++;
    if(clientinfo->pos_last_packet_1 >= clientinfo->packet_list_size_1){
        clientinfo->pos_last_packet_1 = 0;
    }
    return;
}
struct MediaPacket_st getFrameFromList1(struct clientinfo_st *clientinfo){
    struct MediaPacket_st node;
    node.size = 0;
    if(clientinfo->packet_num > 0 && clientinfo->pos_list < clientinfo->packet_list_size){
        memcpy(node.data, clientinfo->packet_list[clientinfo->pos_list].data, clientinfo->packet_list[clientinfo->pos_list].size);
        node.size = clientinfo->packet_list[clientinfo->pos_list].size;
        node.type = clientinfo->packet_list[clientinfo->pos_list].type;
        clientinfo->pos_list++;
        clientinfo->packet_num--;
        if(clientinfo->pos_list >= clientinfo->packet_list_size){
            clientinfo->pos_list = 0;
        }
    }
    return node;
}
struct MediaPacket_st getFrameFromList2(struct clientinfo_st *clientinfo){
    struct MediaPacket_st node;
    node.size = 0;
    if(clientinfo->packet_num_1 > 0 && clientinfo->pos_list_1 < clientinfo->packet_list_size_1){
        memcpy(node.data, clientinfo->packet_list_1[clientinfo->pos_list_1].data, clientinfo->packet_list_1[clientinfo->pos_list_1].size);
        node.size = clientinfo->packet_list_1[clientinfo->pos_list_1].size;
        node.type = clientinfo->packet_list_1[clientinfo->pos_list_1].type;
        clientinfo->pos_list_1++;
        clientinfo->packet_num_1--;
        if(clientinfo->pos_list_1 >= clientinfo->packet_list_size_1){
            clientinfo->pos_list_1 = 0;
        }
    }
    return node;
}
int createClient(struct clientinfo_st *clientinfo, 
    socket_t client_sock_fd, int sig_0, int sig_1, int sig_2, int sig_3, int ture_of_tcp, /*tcp*/
    socket_t server_rtp_fd, socket_t server_rtcp_fd, socket_t server_rtp_fd_1, socket_t server_rtcp_fd_1, char *client_ip, int client_rtp_port, int client_rtcp_port, int client_rtp_port_1, int client_rtcp_port_1 /*udp*/
    )
{
    uint32_t video_ssrc;
    uint32_t audio_ssrc;

    if((clientinfo == NULL) || (client_sock_fd == INVALID_SOCKET)){
        return -1;
    }
    clientinfo->sd = client_sock_fd;
#ifdef RTSP_FILE_SERVER
    clientinfo->send_call_back = sendData; // only file session
#endif
    if(ture_of_tcp == 1){
        clientinfo->transport = RTP_OVER_TCP;
        clientinfo->sig_0 = sig_0;
        clientinfo->sig_1 = sig_1;
        clientinfo->sig_2 = sig_2;
        clientinfo->sig_3 = sig_3;
    }
    else{
        clientinfo->transport = RTP_OVER_UDP;
        memset(clientinfo->client_ip, 0, sizeof(clientinfo->client_ip));
        memcpy(clientinfo->client_ip, client_ip, strlen(client_ip));
        clientinfo->udp_sd_rtp = server_rtp_fd;
        clientinfo->udp_sd_rtcp = server_rtcp_fd;
        clientinfo->udp_sd_rtp_1 = server_rtp_fd_1;
        clientinfo->udp_sd_rtcp_1 = server_rtcp_fd_1;
        clientinfo->client_rtp_port = client_rtp_port;
        clientinfo->client_rtcp_port = client_rtcp_port;
        clientinfo->client_rtp_port_1 = client_rtp_port_1;
        clientinfo->client_rtcp_port_1 = client_rtcp_port_1;
    }
    // video
    clientinfo->rtp_packet = (struct RtpPacket *)malloc(VIDEO_DATA_MAX_SIZE);
    video_ssrc = generateSsrc(client_sock_fd, clientinfo);
    rtpHeaderInit(clientinfo->rtp_packet, 0, 0, 0, RTP_VESION, RTP_PAYLOAD_TYPE_H26X, 0, 0, 0, video_ssrc);
    // audio
    clientinfo->rtp_packet_1 = (struct RtpPacket *)malloc(VIDEO_DATA_MAX_SIZE);
    audio_ssrc = generateSsrc(client_sock_fd, clientinfo->session);
    if(audio_ssrc == video_ssrc){
        audio_ssrc ^= 0x5A5A5A5AU;
        if(audio_ssrc == 0){
            audio_ssrc = 0x2468ACE1U;
        }
    }
    rtpHeaderInit(clientinfo->rtp_packet_1, 0, 0, 0, RTP_VESION, getSessionAudioType(clientinfo->session) == AUDIO_AAC ? RTP_PAYLOAD_TYPE_AAC : RTP_PAYLOAD_TYPE_PCMA, 0, 0, 0, audio_ssrc);

    clientinfo->tcp_header = malloc(sizeof(struct rtp_tcp_header));

    clientinfo->packet_list = (struct MediaPacket_st *)malloc((RING_BUFFER_MAX / 4) * sizeof(struct MediaPacket_st));
    clientinfo->packet_list_size = RING_BUFFER_MAX / 4;
    clientinfo->packet_list_1 = (struct MediaPacket_st *)malloc((RING_BUFFER_MAX / 4) * sizeof(struct MediaPacket_st));
    clientinfo->packet_list_size_1 = RING_BUFFER_MAX / 4;

    clientinfo->playflag = 1;
    return 0;
}
static int sendDataToClient(struct clientinfo_st *clientinfo, char *ptr, int ptr_len, int type){
    int ret = 0;
    struct RtcpPacketInfo rtcp_info;
    enum VIDEO_e video_type = getSessionVideoType(clientinfo->session);
    int sample_rate;
    int channels;
    int profile;
    ret = getSessionAudioInfo(clientinfo->session, &sample_rate, &channels, &profile);
    enum AUDIO_e audio_type = getSessionAudioType(clientinfo->session);
    if(type == VIDEO){
        memset(&rtcp_info, 0, sizeof(rtcp_info));
        if(clientinfo->udp_sd_rtp != INVALID_SOCKET){ // udp
            switch(video_type){
                case VIDEO_H264:
                    ret = rtpSendH264Frame(clientinfo->udp_sd_rtp, NULL, clientinfo->rtp_packet, ptr, ptr_len, 0, -1, clientinfo->client_ip, clientinfo->client_rtp_port, &rtcp_info);
                    break;
                case VIDEO_H265:
                    ret = rtpSendH265Frame(clientinfo->udp_sd_rtp, NULL, clientinfo->rtp_packet, ptr, ptr_len, 0, -1, clientinfo->client_ip, clientinfo->client_rtp_port, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
        else{ // tcp
            switch(video_type){
                case VIDEO_H264:
                    ret = rtpSendH264Frame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet, ptr, ptr_len, 0, clientinfo->sig_0, NULL, -1, &rtcp_info);
                    break;
                case VIDEO_H265:
                    ret = rtpSendH265Frame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet, ptr, ptr_len, 0, clientinfo->sig_0, NULL, -1, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
    }
    else if(type == AUDIO){
        memset(&rtcp_info, 0, sizeof(rtcp_info));
        if(clientinfo->udp_sd_rtp_1 != INVALID_SOCKET){ // udp
            switch(audio_type){
                case AUDIO_AAC:
                    ret = rtpSendAACFrame(clientinfo->udp_sd_rtp_1, NULL, clientinfo->rtp_packet_1, ptr, ptr_len, sample_rate, channels, profile, -1, clientinfo->client_ip, clientinfo->client_rtp_port_1, &rtcp_info);
                    break;
                case AUDIO_PCMA:
                    ret = rtpSendPCMAFrame(clientinfo->udp_sd_rtp_1, NULL, clientinfo->rtp_packet_1, ptr, ptr_len, sample_rate, channels, profile, -1, clientinfo->client_ip, clientinfo->client_rtp_port_1, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
        else{ // tcp
            switch(audio_type){
                case AUDIO_AAC:
                    ret = rtpSendAACFrame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet_1, ptr, ptr_len, sample_rate, channels, profile, clientinfo->sig_2, NULL, -1, &rtcp_info);
                    break;
                case AUDIO_PCMA:
                    ret = rtpSendPCMAFrame(clientinfo->sd, clientinfo->tcp_header, clientinfo->rtp_packet_1, ptr, ptr_len, sample_rate, channels, profile, clientinfo->sig_2, NULL, -1, &rtcp_info);
                    break;
                default:
                    break;
            }
        }
    }
    else{
        return -1;
    }
    if(ret > 0){
        if(maybeSendRtcpSenderReport(clientinfo, type, type == VIDEO ? clientinfo->rtp_packet : clientinfo->rtp_packet_1, &rtcp_info) < 0){
            return -1;
        }
    }
    return ret;
}
#ifdef RTSP_FILE_SERVER
/*Audio and video callback function*/
static void mediaCallBack(void *arg){
    struct session_st *session = (struct session_st *)arg;
    if(session == NULL){
        return;
    }
    int ret = 0;
    mthread_mutex_lock(&session->mut);
    for(int i = 0; i < CLIENTMAX; i++){
        if(session->clientinfo[i].sd != INVALID_SOCKET && session->clientinfo[i].send_call_back != NULL && session->clientinfo[i].playflag == 1){
#ifdef SEND_DATA_EVENT
            session->clientinfo[i].send_call_back(&session->clientinfo[i]);
#else
            // Directly sending audio and video data to the client, not applicable to epoll
            struct clientinfo_st *clientinfo = &session->clientinfo[i];
            if(nowStreamIsVideo(clientinfo->session->media) && (clientinfo->sig_0 != -1 || clientinfo->client_rtp_port != -1)){ // video
                char *ptr = NULL;
                int ptr_len = 0;
                getVideoNALUWithoutStartCode(clientinfo->session->media, &ptr, &ptr_len);
                ret = sendDataToClient(clientinfo, ptr, ptr_len, VIDEO);
                if(ret <= 0){
                    // do nothing, epoll_loop will delete client
                }
            }
            if(nowStreamIsAudio(clientinfo->session->media) && (clientinfo->sig_2 != -1 || clientinfo->client_rtp_port_1 != -1)){ // audio
                char *ptr = NULL;
                int ptr_len = 0;
                getAudioWithoutADTS(clientinfo->session->media, &ptr, &ptr_len);
                ret = sendDataToClient(clientinfo, ptr, ptr_len, AUDIO);
                if(ret <= 0){
                    // do nothing, epoll_loop will delete client
                }
            }
#endif
        }
    }
    mthread_mutex_unlock(&session->mut);
    return;
}
static void reloopCallBack(void *arg){
    struct session_st *session = (struct session_st *)arg;
    if(session == NULL){
        return;
    }
    if(reloop_flag == 1){
        return;
    }
    delFileSession(session_arr[session->pos]);
    return;
}
/*Create a file session and add one client*/
int addFileSession(char *path_filename, 
    socket_t client_sock_fd, int sig_0, int sig_1, int sig_2, int sig_3, int ture_of_tcp, /*tcp*/
    socket_t server_rtp_fd, socket_t server_rtcp_fd, socket_t server_rtp_fd_1, socket_t server_rtcp_fd_1, char *client_ip, int client_rtp_port, int client_rtcp_port, int client_rtp_port_1, int client_rtcp_port_1 /*udp*/
    )
{
    if(path_filename == NULL){
        return -1;
    }
    mthread_mutex_lock(&mut_session);
    int pos = FILEMAX;
    for (int i = 0; i < FILEMAX; i++){
        if (session_arr[i] == NULL){
            if (i < pos){
                pos = i;
            }
            continue;
        }
    }
    struct session_st *session;
    session = malloc(sizeof(struct session_st));
    memset(session, 0, sizeof(struct session_st));
    mthread_mutex_init(&session->mut, NULL);
    session->media = creatMedia(path_filename, mediaCallBack, reloopCallBack, session);

    session->filename = malloc(strlen(path_filename) + 1);
    memset(session->filename, 0, strlen(path_filename) + 1);
    memcpy(session->filename, path_filename, strlen(path_filename));
#ifdef SESSION_DEBUG
    printf("addFileSession:%s client_sock_fd:%d\n", session->filename, client_sock_fd);
#endif
    session->count = 0;
    session_arr[pos] = session;
    session->pos = pos;
    for(int j = 0; j < CLIENTMAX; j++){
        initClient(session_arr[pos], &session_arr[pos]->clientinfo[j]);
    }
    mthread_mutex_lock(&session_arr[pos]->mut);
    session_arr[pos]->count++;
    // add one client
    createClient(&session_arr[pos]->clientinfo[0], 
        client_sock_fd, sig_0, sig_1, sig_2, sig_3, ture_of_tcp, /*tcp*/
        server_rtp_fd, server_rtcp_fd,server_rtp_fd_1, server_rtcp_fd_1, client_ip, client_rtp_port, client_rtcp_port, client_rtp_port_1, client_rtcp_port_1 /*udp*/
        );
    int events = EVENT_ERR|EVENT_RDHUP|EVENT_HUP;
#ifdef SEND_DATA_EVENT
    events |= EVENT_OUT;
#endif
    eventAdd(events, &session_arr[pos]->clientinfo[0]);
    mthread_mutex_unlock(&session_arr[pos]->mut);
    mthread_mutex_unlock(&mut_session);
    return 0;
}
#endif
void* addCustomSession(const char* session_name){
    if(session_name == NULL){
        return NULL;
    }
    int min_free_pos = FILEMAX;
    char path_filename[1024] = {0};
    memcpy(path_filename, mp4Dir, strlen(mp4Dir));
    memcpy(path_filename + strlen(mp4Dir), session_name, strlen(session_name));
    /*Check if the session already exists*/
    mthread_mutex_lock(&mut_session);
    for(int i = 0; i < FILEMAX; i++){
        if(session_arr[i] == NULL){
            if(i < min_free_pos)
                min_free_pos = i;
            continue;
        }
        if(!strncmp(session_arr[i]->filename, path_filename, strlen(path_filename))){
            mthread_mutex_unlock(&mut_session);
            return session_arr[i];
        }
    }
    struct session_st *session;
    session = malloc(sizeof(struct session_st));
    memset(session, 0, sizeof(struct session_st));
    session->is_custom = 1;
    session->audio_type = AUDIO_NONE;
    session->video_type = VIDEO_NONE;

    session->filename = malloc(strlen(path_filename) + 1);
    memset(session->filename, 0, strlen(path_filename) + 1);
    memcpy(session->filename, path_filename, strlen(path_filename));
#ifdef SESSION_DEBUG
    printf("addCustomSession:%s\n", session->filename);
#endif
    mthread_mutex_init(&session->mut, NULL);
    session->count = 0;

    session_arr[min_free_pos] = session;
    session->pos = min_free_pos;
    for(int j = 0; j < CLIENTMAX; j++){
        initClient(session_arr[min_free_pos], &session_arr[min_free_pos]->clientinfo[j]);
    }
    mthread_mutex_unlock(&mut_session);
    return session;
}
static int delAndFreeSession(struct session_st *session){
    if(session == NULL){
        return -1;
    }
    int client_num = 0;
    mthread_mutex_lock(&mut_session);
#ifdef SESSION_DEBUG
    printf("delAndFreeSession:%s\n", session->filename);
#endif
    mthread_mutex_lock(&session->mut);
    for(int i = 0; i < CLIENTMAX; i++){
        if(session->clientinfo[i].sd != INVALID_SOCKET){
            client_num++;
            eventDel(&session->clientinfo[i]);
        }
        clearClient(&session->clientinfo[i]);
    }
   
    if(session->filename){
        free(session->filename);
        session->filename = NULL;
    }
    mthread_mutex_unlock(&session->mut);
    mthread_mutex_destroy(&session->mut);
    session_arr[session->pos] = NULL;
    free(session);
    mthread_mutex_unlock(&mut_session);

    mthread_mutex_lock(&mut_clientcount);
    sum_client -= client_num;
    int cnt = sum_client;
    mthread_mutex_unlock(&mut_clientcount);
#ifdef SESSION_DEBUG
    printf("sum_client:%d\n",cnt);
#endif
    return 0;
}
#ifdef RTSP_FILE_SERVER
/*delete file session*/
void delFileSession(struct session_st *session)
{
    if(session == NULL || session->is_custom == 1){
        return;
    }
    destroyMedia(session->media); // DestroyMedia, do not lock
    delAndFreeSession(session);
    return;
}
#endif
void delCustomSession(void *context){
    if(context == NULL){
        return;
    }
    struct session_st *session = (struct session_st *)context;
    delAndFreeSession(session);
    return;
}
int addVideo(void *context, enum VIDEO_e type){
    if(context == NULL){
        return -1;
    }
    struct session_st *session = (struct session_st *)context;
    session->video_type = type;
    return 0;
}

int addAudio(void *context, enum AUDIO_e type, int profile, int sample_rate, int channels){
    if(context == NULL){
        return -1;
    }
    struct session_st *session = (struct session_st *)context;
    session->audio_type = type;
    session->profile = profile;
    session->sample_rate = sample_rate;
    session->channels = channels;
    return 0;
}
int sendVideoData(void *context, uint8_t *data, int data_len){
    if(context == NULL){
        return -1;
    }
    struct session_st *session = (struct session_st *)context;
    int ret = 0;
    mthread_mutex_lock(&session->mut);
    for (int i = 0; i < CLIENTMAX; i++){
        if (session->clientinfo[i].sd != INVALID_SOCKET && session->clientinfo[i].playflag == 1){
            struct clientinfo_st *clientinfo = &session->clientinfo[i];
#ifdef SEND_DATA_EVENT
            mthread_mutex_lock(&clientinfo->mut_list);
            if(clientinfo->packet_num >= RING_BUFFER_MAX){
                printf("WARING ring buffer too large\n");
            }
            increaseClientList(VIDEO, clientinfo);
            
            if(clientinfo->sig_0 != -1 || clientinfo->client_rtp_port != -1){
                char *ptr = data;
                int ptr_len = data_len;
                memcpy(clientinfo->packet_list[clientinfo->pos_last_packet].data, ptr, ptr_len);
                clientinfo->packet_list[clientinfo->pos_last_packet].size = ptr_len;
                clientinfo->packet_list[clientinfo->pos_last_packet].type = VIDEO;
                clientinfo->packet_num++;
                clientinfo->pos_last_packet++;
                if(clientinfo->pos_last_packet >= clientinfo->packet_list_size){
                    clientinfo->pos_last_packet = 0;
                }
            }
            mthread_mutex_unlock(&clientinfo->mut_list);
#else
            ret = sendDataToClient(clientinfo, data, data_len, VIDEO);
            if(ret <= 0){
                // do nothing, epoll_loop will delete client
            }
#endif
        }
    }
    mthread_mutex_unlock(&session->mut);
    return 0;
}

int sendAudioData(void *context, uint8_t *data, int data_len){
    if(context == NULL){
        return -1;
    }
    struct session_st *session = (struct session_st *)context;
    int ret = 0;
    mthread_mutex_lock(&session->mut);
    for (int i = 0; i < CLIENTMAX; i++){
        if (session->clientinfo[i].sd != INVALID_SOCKET && session->clientinfo[i].playflag == 1){
            struct clientinfo_st *clientinfo = &session->clientinfo[i];
#ifdef SEND_DATA_EVENT
            mthread_mutex_lock(&clientinfo->mut_list);
            if(clientinfo->packet_num >= RING_BUFFER_MAX){
                printf("WARING ring buffer too large\n");
            }
            increaseClientList(AUDIO, clientinfo);
            
            if(clientinfo->sig_2 != -1 || clientinfo->client_rtp_port_1 != -1){ 
                char *ptr = data;
                int ptr_len = data_len;
                if(clientinfo->client_rtp_port_1 != -1){ // UDP, Use different queues for audio and video
                    memcpy(clientinfo->packet_list_1[clientinfo->pos_last_packet_1].data, ptr, ptr_len);
                    clientinfo->packet_list_1[clientinfo->pos_last_packet_1].size = ptr_len;
                    clientinfo->packet_list_1[clientinfo->pos_last_packet_1].type = AUDIO;
                    clientinfo->packet_num_1++;
                    clientinfo->pos_last_packet_1++;
                    if(clientinfo->pos_last_packet_1 >= clientinfo->packet_list_size_1){
                        clientinfo->pos_last_packet_1 = 0;
                    }
                }
                else if(clientinfo->sig_2 != -1){ // TCP, audio and video use the same queue
                    memcpy(clientinfo->packet_list[clientinfo->pos_last_packet].data, ptr, ptr_len);
                    clientinfo->packet_list[clientinfo->pos_last_packet].size = ptr_len;
                    clientinfo->packet_list[clientinfo->pos_last_packet].type = AUDIO;
                    clientinfo->packet_num++;
                    clientinfo->pos_last_packet++;
                    if(clientinfo->pos_last_packet >= clientinfo->packet_list_size){
                        clientinfo->pos_last_packet = 0;
                    }
                }
            }
            mthread_mutex_unlock(&clientinfo->mut_list);
#else
            ret = sendDataToClient(clientinfo, data, data_len, AUDIO);
            if(ret <= 0){
                // do nothing, epoll_loop will delete client
            }
#endif
        }
    }
    mthread_mutex_unlock(&session->mut);
    return 0;
}
int getSessionAudioType(struct session_st *session){
    if(session == NULL){
        return AUDIO_NONE;
    }
    if(session->is_custom == 0){
#ifdef RTSP_FILE_SERVER
        return getAudioType(session->media);
#endif
    }
    else{
        return session->audio_type;
    }
    return AUDIO_NONE;
}
int getSessionAudioInfo(struct session_st *session, int *sample_rate, int *channels, int *profile){
    if(session == NULL){
        return -1;
    }
    if(session->is_custom == 0){
#ifdef RTSP_FILE_SERVER
        struct audioinfo_st audioinfo = getAudioInfo(session->media);
        *sample_rate = audioinfo.sample_rate;
        *channels = audioinfo.channels;
        *profile = audioinfo.profile;
#endif
    }
    else{
        *sample_rate = session->sample_rate;
        *channels = session->channels;
        *profile = session->profile;
    }
    return 0;
}

int getSessionVideoType(struct session_st *session){
    if(session == NULL){
        return VIDEO_NONE;
    }
    if(session->is_custom == 0){
#ifdef RTSP_FILE_SERVER
        return getVideoType(session->media);
#endif
    }
    else{
        return session->video_type;
    }
    return VIDEO_NONE;
}
int sessionIsExist(char* suffix){
    char path_filename[1024] = {0};
    memcpy(path_filename, mp4Dir, strlen(mp4Dir));
    memcpy(path_filename + strlen(mp4Dir), suffix, strlen(suffix));
    mthread_mutex_lock(&mut_session);
    for (int i = 0; i < FILEMAX; i++){
        if ((session_arr[i] == NULL) || (session_arr[i]->filename == NULL)){
            continue;
        }
        if (!strncmp(session_arr[i]->filename, path_filename, strlen(path_filename))){
            mthread_mutex_unlock(&mut_session);
            return 1;
        }
    }
    mthread_mutex_unlock(&mut_session);
#ifdef RTSP_FILE_SERVER
    FILE *file = fopen(path_filename, "r");
    if (file) {
        fclose(file);
        return 1;
    } else {
        return 0;
    }
#endif
    return 0;
}
int sessionGenerateSDP(char *suffix, char *localIp, char *buffer, int buffer_len){
    if(suffix == NULL){
        return -1;
    }
    char path_filename[1024] = {0};
    memcpy(path_filename, mp4Dir, strlen(mp4Dir));
    memcpy(path_filename + strlen(mp4Dir), suffix, strlen(suffix));
    mthread_mutex_lock(&mut_session);
    for (int i = 0; i < FILEMAX; i++){   
        if((session_arr[i] == NULL) || (session_arr[i]->filename == NULL)){
            continue;
        }
        if(!strncmp(session_arr[i]->filename, path_filename, strlen(path_filename))){
            if(session_arr[i]->is_custom == 0){
#ifdef RTSP_FILE_SERVER
                mthread_mutex_unlock(&mut_session);
                return generateSDP(path_filename, localIp, buffer, buffer_len);
#endif
            }
            else{
                mthread_mutex_unlock(&mut_session);
                return generateSDPExt(localIp, buffer, buffer_len, session_arr[i]->video_type, 
                                    session_arr[i]->audio_type, session_arr[i]->sample_rate, session_arr[i]->profile, session_arr[i]->channels);
            }
        }
    }
    mthread_mutex_unlock(&mut_session);
#ifdef RTSP_FILE_SERVER
    return generateSDP(path_filename, localIp, buffer, buffer_len);
#endif
    return -1;
}

/*add one client*/
int addClient(char* suffix, 
    socket_t client_sock_fd, int sig_0, int sig_1, int sig_2, int sig_3, int ture_of_tcp, /*tcp*/
    char *client_ip, int client_rtp_port, int client_rtcp_port, int client_rtp_port_1, int client_rtcp_port_1, /*client udp info*/
    socket_t server_udp_socket_rtp, socket_t server_udp_socket_rtcp, socket_t server_udp_socket_rtp_1, socket_t server_udp_socket_rtcp_1 /*udp socket*/
    )
{
#ifdef SESSION_DEBUG
    printf("sig_0:%d, sig_1:%d, sig_2:%d, sig_3:%d, ture_of_tcp:%d, client_ip:%s, client_rtp_port:%d, client_rtcp_port:%d, client_rtp_port_1:%d, client_rtcp_port_1:%d, server_udp_socket_rtp:%d server_udp_socket_rtcp:%d server_udp_socket_rtp_1:%d,server_udp_socket_rtcp_1:%d\n",
           sig_0, sig_1, sig_2, sig_3, ture_of_tcp, client_ip, client_rtp_port, client_rtcp_port, client_rtp_port_1, client_rtcp_port_1, server_udp_socket_rtp, server_udp_socket_rtcp, server_udp_socket_rtp_1, server_udp_socket_rtcp_1);
#endif
    int istrueflag = 0;
    int pos = 0;
    int fps;
    char path_filename[1024] = {0};
    memcpy(path_filename, mp4Dir, strlen(mp4Dir));
    memcpy(path_filename + strlen(mp4Dir), suffix, strlen(suffix));
    /*Check if the session already exists*/
    mthread_mutex_lock(&mut_session);
    for(int i = 0; i < FILEMAX; i++){
        if(session_arr[i] == NULL){
            continue;
        }
        /* 找到sessionName对应的session */
        if(!strncmp(session_arr[i]->filename, path_filename, strlen(path_filename))){ // The session exists, add the client to the client queue of the session
            mthread_mutex_lock(&session_arr[i]->mut);
            istrueflag = 1;
            pos = i;
            /* 从session中clientinfo列表里找到还没用的clientinfo索引 */
            int posofclient = get_free_clientinfo(pos);
            if(posofclient < 0){ // Exceeding the maximum number of clients supported by a session, a session can support a maximum of FileMAX (1024) clients
                printf("over client maxnum\n");
                mthread_mutex_unlock(&session_arr[pos]->mut);
                mthread_mutex_unlock(&mut_session);
                return -1;
            }
            createClient(&session_arr[pos]->clientinfo[posofclient], 
                client_sock_fd, sig_0, sig_1, sig_2, sig_3, ture_of_tcp, /*tcp*/
                server_udp_socket_rtp, server_udp_socket_rtcp,server_udp_socket_rtp_1, server_udp_socket_rtcp_1, client_ip, client_rtp_port, client_rtcp_port, client_rtp_port_1, client_rtcp_port_1 /*udp*/
                );
            int events = EVENT_ERR|EVENT_RDHUP|EVENT_HUP;
#ifdef SEND_DATA_EVENT
            events |= EVENT_OUT;
#endif
            eventAdd(events, &session_arr[pos]->clientinfo[posofclient]);
            session_arr[pos]->count++;
#ifdef SESSION_DEBUG
            printf("append client ok fd:%d\n", session_arr[pos]->clientinfo[posofclient].sd);
#endif
            mthread_mutex_unlock(&session_arr[i]->mut);
            break;
        }
    }
    mthread_mutex_unlock(&mut_session);
    if(istrueflag == 0){ // create a new file session, not custom session
#ifdef RTSP_FILE_SERVER
        int ret = addFileSession(path_filename, client_sock_fd, sig_0, sig_1, sig_2, sig_3, ture_of_tcp, server_udp_socket_rtp, server_udp_socket_rtcp, server_udp_socket_rtp_1, server_udp_socket_rtcp_1, client_ip, client_rtp_port, client_rtcp_port, client_rtp_port_1, client_rtcp_port_1);
        if (ret < 0)
        {

            return -1;
        }
#else
        return -1;
#endif
    }
    mthread_mutex_lock(&mut_clientcount);
    sum_client++;
    mthread_mutex_unlock(&mut_clientcount);
    return 0;
}
int getClientNum()
{
    int sum;
    mthread_mutex_lock(&mut_clientcount);
    sum = sum_client;
    mthread_mutex_unlock(&mut_clientcount);
    return sum;
}

int getSessionClientNum(void *context)
{
    struct session_st *session = (struct session_st *)context;
    int count;
    if (session == NULL) {
        return -1;
    }
    mthread_mutex_lock(&session->mut);
    count = session->count;
    mthread_mutex_unlock(&session->mut);
    return count;
}
