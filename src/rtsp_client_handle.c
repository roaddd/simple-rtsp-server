#include "rtsp_client_handle.h"
#define BUF_MAX_SIZE (4 * 1024)
#define RTSP_DEBUG

static int parse_rtsp_suffix(const char *url, char *suffix, size_t suffix_size)
{
    const char *p;
    if (!url || !suffix || suffix_size == 0) {
        return -1;
    }
    suffix[0] = '\0';
    p = strstr(url, "rtsp://");
    if (!p) {
        return -1;
    }
    p = strchr(p + strlen("rtsp://"), '/');
    if (!p || !p[1]) {
        return -1;
    }
    strncpy(suffix, p + 1, suffix_size - 1);
    suffix[suffix_size - 1] = '\0';
    return 0;
}

/* SETUP 必须指向具体 track，形如 rtsp://host:port/session/track0。 */
static int parse_rtsp_setup_url(const char *url,
                                char *session_name,
                                size_t session_name_size,
                                char *track,
                                size_t track_size)
{
    char suffix[512] = {0};
    char *slash;
    if (!session_name || session_name_size == 0 || !track || track_size == 0) {
        return -1;
    }
    session_name[0] = '\0';
    track[0] = '\0';
    if (parse_rtsp_suffix(url, suffix, sizeof(suffix)) != 0) {
        return -1;
    }
    slash = strrchr(suffix, '/');
    if (!slash || !slash[1]) {
        return -1;
    }
    *slash = '\0';
    if (suffix[0] == '\0') {
        return -1;
    }
    strncpy(session_name, suffix, session_name_size - 1);
    session_name[session_name_size - 1] = '\0';
    strncpy(track, slash + 1, track_size - 1);
    track[track_size - 1] = '\0';
    return 0;
}

/* 非 track0/track1 的 SETUP 不能默认落到音频，否则会创建错误的 RTP 通道。 */
static int is_valid_setup_track(const char *track)
{
    return track && (!strcmp(track, "track0") || !strcmp(track, "track1"));
}

static void close_pending_setup_sockets(socket_t *fds, size_t count)
{
    size_t i;
    if (!fds) {
        return;
    }
    for (i = 0; i < count; ++i) {
        if (fds[i] != INVALID_SOCKET) {
            closeSocket(fds[i]);
            fds[i] = INVALID_SOCKET;
        }
    }
}

/**
 * @description: 处理客户端线程，在新客户端 TCP 连接建立后启动，
 * 负责首轮 RTSP 交互（OPTIONS/DESCRIBE/SETUP/PLAY）、鉴权、解析 Transport、最后调用 addClient(...) 把客户端加入会话
 * @param {void} *arg
 * @return {*}
 */
void *doClientThd(void *arg)
{
#if defined(__linux__) || defined(__linux)
    signal(SIGINT, sig_handler);
    signal(SIGQUIT, sig_handler);
    signal(SIGKILL, sig_handler);
#endif
    struct thd_arg_st *arg_thd = (struct thd_arg_st *)arg;
    socket_t client_sock_fd = arg_thd->client_sock_fd;
    char *client_ip = arg_thd->client_ip;
    int client_port = arg_thd->client_port;
    char suffix[100] = {0};
    char url_setup[100] = {0};
    char track[1024] = {0};
    char url_play[1024] = {0};
    char local_ip[40] = {0};
    int cseq = 0;
    char recv_buf[BUF_MAX_SIZE] = {0};
    char send_buf[BUF_MAX_SIZE] = {0};
    // rtp_over_tcp
    int sig_0 = -1;
    int sig_1 = -1;
    int sig_2 = -1;
    int sig_3 = -1;
    int ture_of_rtp_tcp = 0;
    // rtp_over_udp
    int client_rtp_port = -1;
    int client_rtcp_port = -1;
    int client_rtp_port_1 = -1;
    int client_rtcp_port_1 = -1;
    int server_rtp_port = -1;
    int server_rtcp_port = -1;
    int server_rtp_port_1 = -1;
    int server_rtcp_port_1 = -1;
    socket_t server_udp_socket_rtp_fd = INVALID_SOCKET;
    socket_t server_udp_socket_rtcp_fd = INVALID_SOCKET;
    socket_t server_udp_socket_rtp_1_fd = INVALID_SOCKET;
    socket_t server_udp_socket_rtcp_1_fd = INVALID_SOCKET;

    char ch = '/';
    int findflag = 0;

    char *realm = "simple-rtsp-server";
    char nonce[33] = {0};
    char session_id[512] = {0};

    int used_bytes = 0;
    int pos = 0;
    int total_len = 0;
    int ret = 0;
    generateNonce(nonce, sizeof(nonce));
    generateSessionId(session_id, sizeof(session_id));
    while(1){
        int recv_len = recvWithTimeout(client_sock_fd, recv_buf + pos, BUF_MAX_SIZE - pos, 0);
        if (recv_len <= 0)
            goto out;
        total_len += recv_len;
        recv_buf[total_len] = '\0';
#ifdef RTSP_DEBUG
        printf("---------------C->S--------------\n");
        printf("%s", recv_buf);
#endif
        struct rtsp_request_message_st request_message;
        memset(&request_message, 0, sizeof(struct rtsp_request_message_st));
        int parse_used = parseRtspRequest(recv_buf, total_len, &request_message);
        if(parse_used < 0){
            goto out;
        }
        used_bytes += parse_used;
        // dumpRequestMessage(&request_message);
        char *CSeq = findValueByKey(&request_message, "CSeq");
        if(CSeq == NULL){
            used_bytes -= parse_used;
            goto need_more_data;
        }
        cseq = atoi(CSeq);
        if(arg_thd->auth == 1){
            // authorization
            if(!strcmp(request_message.method, "SETUP") || !strcmp(request_message.method, "DESCRIBE") || !strcmp(request_message.method, "PLAY") || !strcmp(request_message.method, "TEARDOWN")){
                char *Authorization = findValueByKey(&request_message, "Authorization");
                if(Authorization == NULL){
                    handleCmd_Unauthorized(send_buf, cseq, realm, nonce);
                    goto need_more_data;
                }
                else{
                    AuthorizationInfo *auth_info = findAuthorizationByValue((const char *)Authorization);
                    // printf("nonce:%s\n", auth_info->nonce);
                    // printf("realm:%s\n", auth_info->realm);
                    // printf("response:%s\n", auth_info->response);
                    // printf("uri:%s\n", auth_info->uri);
                    // printf("username:%s\n", auth_info->username);
                    ret = authorizationVerify(arg_thd->user_name, arg_thd->password, realm, nonce, auth_info->uri, request_message.method, auth_info->response);
                    freeAuthorizationInfo(auth_info);
                    if(ret < 0){
                        handleCmd_Unauthorized(send_buf, cseq, realm, nonce);
                        goto out;
                    }
                }
            }
        }
        /* SETUP:RTP_OVER_TCP or RTP_OVER_UDP */
        if(!strcmp(request_message.method, "SETUP")){
            memset(track, 0, sizeof(track));
            memset(url_setup, 0, sizeof(url_setup));
            /* 拒绝裸 session SETUP，避免 rtsp://.../live_sub 被误判为 track1。 */
            if (parse_rtsp_setup_url(request_message.url, suffix, sizeof(suffix), track, sizeof(track)) != 0 ||
                !is_valid_setup_track(track)) {
                printf("invalid SETUP url: %s\n", request_message.url);
                handleCmd_404(send_buf, cseq);
                goto out;
            }
            /* OPTIONS/DESCRIBE 被跳过时，SETUP 仍要独立确认 session 存在。 */
            ret = sessionIsExist(suffix);
            if (ret <= 0) {
                printf("The resource does not exist\n");
                handleCmd_404(send_buf, cseq);
                goto out;
            }
            findflag = 1;
            char *Transport = findValueByKey(&request_message, "Transport");
            if(Transport == NULL){
                used_bytes -= parse_used;
                goto need_more_data;
            }

            if(!strncmp(Transport, "RTP/AVP/TCP", strlen("RTP/AVP/TCP"))){

                if(strcmp(track, "track0") == 0){
                    sscanf(Transport, "RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n", &sig_0, &sig_1);
                }
                else{
                    sscanf(Transport, "RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n", &sig_2, &sig_3);
                }
                ture_of_rtp_tcp = 1;
            }
            else if(!strncmp(Transport, "RTP/AVP/UDP", strlen("RTP/AVP/UDP"))){
                if(strcmp(track, "track0") == 0){
                    sscanf(Transport, "RTP/AVP/UDP;unicast;client_port=%d-%d\r\n", &client_rtp_port, &client_rtcp_port);
                }
                else{
                    sscanf(Transport, "RTP/AVP/UDP;unicast;client_port=%d-%d\r\n", &client_rtp_port_1, &client_rtcp_port_1);
                }
            }
            else if(!strncmp(Transport, "RTP/AVP", strlen("RTP/AVP"))){

                if(strcmp(track, "track0") == 0){
                    sscanf(Transport, "RTP/AVP;unicast;client_port=%d-%d\r\n", &client_rtp_port, &client_rtcp_port);
                }
                else{
                    sscanf(Transport, "RTP/AVP;unicast;client_port=%d-%d\r\n", &client_rtp_port_1, &client_rtcp_port_1);
                }
            }
        
        }
        if(!strcmp(request_message.method, "OPTIONS")){
            if (parse_rtsp_suffix(request_message.url, suffix, sizeof(suffix)) != 0) {
                handleCmd_404(send_buf, cseq);
                goto out;
            }
            ret = sessionIsExist(suffix);
            findflag = 1;
            if(ret <= 0){ // The resource does not exist
                printf("The resource does not exist\n");
                handleCmd_404(send_buf, cseq);
                goto out;
            }
            else{
                handleCmd_OPTIONS(send_buf, cseq);
            }
        }
        else if(!strcmp(request_message.method, "DESCRIBE")){
            if(findflag == 0){
                if (parse_rtsp_suffix(request_message.url, suffix, sizeof(suffix)) != 0) {
                    handleCmd_404(send_buf, cseq);
                    goto out;
                }
                ret = sessionIsExist(suffix);
                if (ret <= 0){ // The resource does not exist
                    printf("The resource does not exist\n");
                    handleCmd_404(send_buf, cseq);
                    goto out;
                }
                findflag = 1;
            }
            char sdp[1024];
            char localIp[100];
            sscanf(request_message.url, "rtsp://%[^:]:", localIp);
            ret = sessionGenerateSDP(suffix, localIp, sdp, sizeof(sdp));
            if(ret < 0){
                handleCmd_500(send_buf, cseq);
                goto out;
            }
            handleCmd_DESCRIBE(send_buf, cseq, request_message.url, sdp);
        }
        else if(!strcmp(request_message.method, "SETUP") && ture_of_rtp_tcp == 0){ // RTP_OVER_UDP
            sscanf(request_message.url, "rtsp://%[^:]:", local_ip);
            if(strcmp(track, "track0") == 0){
                /* 创建两个 RTP 套接字，并通过 fd1/fd2 返回其描述符，同时把分配的端口号写入 port1 和 port2 */
                createRtpSockets(&server_udp_socket_rtp_fd, &server_udp_socket_rtcp_fd, &server_rtp_port, &server_rtcp_port);
                handleCmd_SETUP_UDP(send_buf, cseq, client_rtp_port, client_rtcp_port, server_rtp_port, server_rtcp_port, session_id);
            }
            else{
                createRtpSockets(&server_udp_socket_rtp_1_fd, &server_udp_socket_rtcp_1_fd, &server_rtp_port_1, &server_rtcp_port_1);
                handleCmd_SETUP_UDP(send_buf, cseq, client_rtp_port_1, client_rtcp_port_1, server_rtp_port_1, server_rtcp_port_1, session_id);
            }
        }
        else if(!strcmp(request_message.method, "SETUP") && ture_of_rtp_tcp == 1){ // RTP_OVER_TCP
            sscanf(request_message.url, "rtsp://%[^:]:", local_ip);
            if(strcmp(track, "track0") == 0){
                handleCmd_SETUP_TCP(send_buf, cseq, local_ip, client_ip, sig_0, sig_1, session_id);
            }
            else{
                handleCmd_SETUP_TCP(send_buf, cseq, local_ip, client_ip, sig_2, sig_3, session_id);
            }
        }
        else if(!strcmp(request_message.method, "PLAY")){
            memset(url_play, 0, sizeof(url_play));
            memset(track, 0, sizeof(track));
            strcpy(url_play, request_message.url);
            if (parse_rtsp_suffix(request_message.url, suffix, sizeof(suffix)) != 0 ||
                sessionIsExist(suffix) <= 0) {
                printf("invalid PLAY url: %s\n", request_message.url);
                handleCmd_404(send_buf, cseq);
                goto out;
            }
            handleCmd_PLAY(send_buf, cseq, url_play, session_id);
        }
        else if(!strcmp(request_message.method, "TEARDOWN")){
            char *Session = findValueByKey(&request_message, "Session");
            handleCmd_General(send_buf, cseq, Session ? Session : session_id);
            goto out;
        }
        else{
            goto out;
        }
need_more_data:
        if(strlen(send_buf) > 0){
#ifdef RTSP_DEBUG
            printf("---------------S->C--------------\n");
            printf("%s", send_buf);
#endif
            sendWithTimeout(client_sock_fd, (const char*)send_buf, strlen(send_buf), 0);
            memset(send_buf, 0, sizeof(send_buf));
        }
        memmove(recv_buf, recv_buf + used_bytes, total_len - used_bytes);
        total_len -= used_bytes;
        pos = total_len;
        used_bytes = 0;
        if(!strcmp(request_message.method, "PLAY")){
            if (suffix[0] == '\0') {
                printf("invalid PLAY session: empty suffix\n");
                goto out;
            }
            ret = addClient(suffix, client_sock_fd, sig_0, sig_1, sig_2, sig_3, ture_of_rtp_tcp, client_ip, client_rtp_port, client_rtcp_port, client_rtp_port_1, client_rtcp_port_1,
                                server_udp_socket_rtp_fd, server_udp_socket_rtcp_fd, server_udp_socket_rtp_1_fd, server_udp_socket_rtcp_1_fd);
            if (ret < 0)
                goto out;
            int sum = getClientNum();
#ifdef RTSP_DEBUG
            printf("sum_client:%d\n\n", sum);
#endif
            goto over;
        }
    }
out:
    {
        socket_t pending_fds[] = {
            server_udp_socket_rtp_fd,
            server_udp_socket_rtcp_fd,
            server_udp_socket_rtp_1_fd,
            server_udp_socket_rtcp_1_fd
        };
        close_pending_setup_sockets(pending_fds, sizeof(pending_fds) / sizeof(pending_fds[0]));
        server_udp_socket_rtp_fd = pending_fds[0];
        server_udp_socket_rtcp_fd = pending_fds[1];
        server_udp_socket_rtp_1_fd = pending_fds[2];
        server_udp_socket_rtcp_1_fd = pending_fds[3];
    }
    if(strlen(send_buf) > 0){
#ifdef RTSP_DEBUG
        printf("---------------S->C--------------\n");
        printf("%s", send_buf);
#endif
        sendWithTimeout(client_sock_fd, (const char*)send_buf, strlen(send_buf), 0);
    }
    closeSocket(client_sock_fd);
    free(arg);
    return NULL;
over:
    free(arg);
    return NULL;
}
