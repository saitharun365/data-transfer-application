#include "pch.h"
#include "SenderSocket.h" 

SenderSocket::SenderSocket() {
    start_time = clock();
    current_time = clock();
    syn_start_time = clock();
    syn_end_time = clock();
    fin_start_time = clock();
    fin_end_time = clock();
    connection_open = false;
    rto = 1;
    current_seq = 0;
    current_ack = 0;
    estimated_rtt = 0;
    dev_rtt = 0;
    current_base = 0;
    average_rate = 0;
    base = 0;
    window_size = 0;
    eventQuit = CreateEvent(NULL, false, false, NULL);
    close_called = false;
    retry_count = 0;
    duplicate_ack = 0;
    fast_retransmit = 0;
    last_released = 0;
    effective_window = 0;
    exception_exit = false;
}

SenderSocket:: ~SenderSocket() {
    delete[] packets_buffer;
    WSACleanup();
}

UINT SenderSocket::stats_thread(LPVOID pParam)
{
    SenderSocket* ss = ((SenderSocket*)pParam);
    int count = 0;
    int last_pkt_acked = 0;
    while (WaitForSingleObject(ss->eventQuit, 2000) == WAIT_TIMEOUT)
    {
        double speed = ((ss->current_ack - last_pkt_acked) * 8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader))) / (2 * 1000000.0);
        double current_speed_total = ss->average_rate * count;
        count++;
        ss->average_rate = (current_speed_total + speed) / (double)count;
        last_pkt_acked = ss->current_ack;

        printf("[%3d] B %6d (%6.1f MB) N %6d T %3d F %3d W %5d S %6.3f Mbps RTT %.3f\n", (clock() - ss->start_time) / 1000, ss->current_base, ((float)ss->bytes_acked) / 1000000.0, ss->current_seq, ss->timed_out_packets, ss->fast_retransmit, ss->effective_window, speed, ss->estimated_rtt);
    }

    return 0;
}

int SenderSocket::sendData(int pkt_no) {
    int iterator = (pkt_no % window_size);

    packets_buffer[iterator].txTime = clock();

    if (sendto(sock, (char*)packets_buffer[iterator].pd, packets_buffer[iterator].size, 0, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
    {
        //printf("failed sendto with %d\n", WSAGetLastError());
        return FAILED_SEND;
    };
    return STATUS_OK;
}

int SenderSocket::receiveData(bool& retransmitted) {

    struct sockaddr_in res_server;
    int res_server_size = sizeof(res_server);
    char* res_buf = new char[sizeof(ReceiverHeader)];

    int bytes_received = recvfrom(sock, res_buf, sizeof(ReceiverHeader), 0, (struct sockaddr*)&res_server, &res_server_size);

    if (bytes_received == SOCKET_ERROR)
    {
        printf("failed recvfrom with %d\n", WSAGetLastError());
        return FAILED_RECV;
    };

    ReceiverHeader* rh = (ReceiverHeader*)res_buf;

    if (rh->ackSeq > current_base) {
        current_ack = rh->ackSeq;
        int pkts_acked = current_ack - current_base;
        bytes_acked += (pkts_acked * MAX_PKT_SIZE);
        current_base = current_ack;
        effective_window = min(window_size, rh->recvWnd);
        int new_released = current_base + effective_window - last_released;
        ReleaseSemaphore(empty, new_released, NULL);
        last_released += new_released;

        if (retry_count == 0)
        {
            int slot = (current_ack - 1) % window_size;
            double packet_time = (clock() - packets_buffer[slot].txTime) / 1000.0;
            estimated_rtt = (((1 - ALPHA) * estimated_rtt) + (ALPHA * packet_time));
            dev_rtt = (((1 - BETA) * dev_rtt) + (BETA * abs(packet_time - estimated_rtt)));
            rto = (estimated_rtt + (4 * max(dev_rtt, 0.01)));
        }

        retry_count = 0;
        duplicate_ack = 0;

    }
    else if (rh->ackSeq == current_base)
    {
        duplicate_ack++;
        if (duplicate_ack == 3)
        {
            sendData(current_base);
            retry_count++;
            fast_retransmit++;
            retransmitted = true;
        }
    }
    
    return STATUS_OK;
    
}

UINT SenderSocket::worker_thread(LPVOID pParam)
{
    SenderSocket* ss = ((SenderSocket*)pParam);

    int kernelBuffer = 20e6; // 20 meg
    if (setsockopt(ss->sock, SOL_SOCKET, SO_RCVBUF, (const char *) & kernelBuffer, sizeof(int)) == SOCKET_ERROR)
        return 0;
        
    kernelBuffer = 20e6; // 20 meg
    if (setsockopt(ss->sock, SOL_SOCKET, SO_SNDBUF, (const char*) &kernelBuffer, sizeof(int)) == SOCKET_ERROR)
        return 0;
        
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    int next_pkt = 0;
    int timeout = ss->rto * 1000;
    int timerExpire = clock() + (ss->rto * 1000) * CLOCKS_PER_SEC;
    bool retransmitted = false;
    HANDLE events[] = { ss->data_received_event, ss->full, ss->eventQuit };

    while (!ss->close_called || (ss->current_ack < ss->current_seq))
    {
        if (next_pkt > ss->current_base)
        {
            timeout = (ss->packets_buffer[ss->current_base % ss->window_size].txTime - clock()) / (CLOCKS_PER_SEC)+ (1000.0 * ss->rto);
        }
        else
        {
            timeout = INFINITE;
        }

        if (ss->retry_count > MAX_DATA_ATTEMPTS)
        {
            ss->exception_exit = true;
            printf("Max retry attempts exceeded\n");
        }

        int ret = WaitForMultipleObjects(3, events, false, timeout); 

        switch (ret) {
            case WAIT_TIMEOUT:
                ss->timed_out_packets++;
                ss->retry_count++;
                retransmitted = true;
                ss->sendData(ss->current_base);
                break;
            case WAIT_OBJECT_0:
                ss->receiveData(retransmitted);
                break;
            case WAIT_OBJECT_0 + 1:
                ss->sendData(next_pkt);
                next_pkt++;
                break;
            case WAIT_OBJECT_0 + 2:
                ss->close_called = true;
                break;
            default:
                break;
        }

        if (retransmitted)
        {
            retransmitted = false;
            ss->packets_buffer[ss->current_base % ss->window_size].txTime = clock();
        }

    }

    return 0;
}

int SenderSocket::Open(char* host, int port, int senderWindow, LinkProperties* lp)
{
    if (connection_open) return ALREADY_CONNECTED;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET)
    {
        printf("socket() generated error %d\n", WSAGetLastError());
        return -1;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(0);
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
    {
        printf("bind() generated error %d\n", WSAGetLastError());
        return -1;
    }

    struct hostent* remote;

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port); 

    DWORD IP = inet_addr(host);
    if (IP == INADDR_NONE)
    {
        // if not a valid IP, then do a DNS lookup
        if ((remote = gethostbyname(host)) == NULL)
        {
            current_time = clock() - start_time;
            printf("[%.3f] --> target %s is invalid\n", (float)(current_time / (float)1000), host);
            return INVALID_NAME;
        }
        else { // take the first IP address and copy into sin_addr
            memcpy((char*)&(server.sin_addr), remote->h_addr, remote->h_length);
        }
    }
    else
    {
        server.sin_addr.S_un.S_addr = IP;
    }

    SenderSynHeader* ssh = new SenderSynHeader();

    ssh->sdh.flags.reserved = 0;
    ssh->sdh.flags.SYN = 1;
    ssh->sdh.flags.FIN = 0;
    ssh->sdh.flags.ACK = 0;
    ssh->sdh.seq = 0;

    ssh->lp = *lp;
    ssh->lp.bufferSize = senderWindow + MAX_SYN_ATTEMPTS;

    rto = max(1, 2 * ssh->lp.RTT);

    syn_start_time = clock() - start_time;
    for (int i = 0; i < MAX_SYN_ATTEMPTS; i++) {
        float packet_send_time = clock();
        current_time = clock() - start_time;
        //printf("[%.3f] --> SYN 0 (attempt %d of %d, RTO %.3f) to %s\n", (float)(current_time / (float)1000), i+1, MAX_SYN_ATTEMPTS, rto, inet_ntoa(server.sin_addr));

        if (sendto(sock, (char*)ssh, sizeof(SenderSynHeader), 0, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
        {
            printf("[%.3f] --> failed sendto with %d\n", (float)(current_time / (float)1000), WSAGetLastError());
            return FAILED_SEND;
        };

        fd_set fd;
        FD_ZERO(&fd); // clear the set
        FD_SET(sock, &fd); // add your socket to the set
        timeval tp;
        tp.tv_sec = rto;
        tp.tv_usec = (rto - floor(rto)) * 1000000;

        struct sockaddr_in res_server;
        int res_server_size = sizeof(res_server);
        char* res_buf = new char[sizeof(ReceiverHeader)];

        int available = select(0, &fd, NULL, NULL, &tp);

        if (available == 0) {
            continue;
        }

        if (available == SOCKET_ERROR)
        {
            current_time = clock() - start_time;
            printf("[%.3f] <-- failed recvfrom with %d\n", (float)(current_time / (float)1000), WSAGetLastError());
            return FAILED_RECV;
        };

        if (available > 0)
        {
            int bytes_received = recvfrom(sock, res_buf, sizeof(ReceiverHeader), 0, (struct sockaddr*)&res_server, &res_server_size);

            if (bytes_received == SOCKET_ERROR)
            {
                current_time = clock() - start_time;
                printf("[%.3f] <-- failed recvfrom with %d\n", (float)(current_time / (float)1000), WSAGetLastError());
                return FAILED_RECV;
            };

            ReceiverHeader* rh = (ReceiverHeader*)res_buf;

            if (rh->flags.SYN == 1 && rh->flags.ACK == 1) {
                clock_t temp = current_time;
                current_time = clock() - start_time;
                syn_end_time = current_time;
                rto = ((float)((current_time) -temp) * 3) / 1000;
                //printf("[%.3f] <-- SYN-ACK 0 window %d; setting initial RTO to %.3f\n", (float)(current_time / (float)1000), rh->recvWnd, rto);
                connection_open = true;

                if (i == 0)
                {
                    float packet_time = clock() - packet_send_time;
                    estimated_rtt = (((1 - ALPHA) * estimated_rtt) + (ALPHA * packet_time)) / 1000;
                    dev_rtt = (((1 - BETA) * dev_rtt) + (BETA * abs(packet_time - estimated_rtt))) / 1000;
                    rto = (estimated_rtt + (4 * max(dev_rtt, 0.01)));
                }

                packets_buffer = new Packet[senderWindow];
                stats_thread_handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)stats_thread, this, 0, NULL);
                worker_thread_handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)worker_thread, this, 0, NULL);
                window_size = senderWindow;
                data_received_event = CreateEventA(0, FALSE, FALSE, 0);
                last_released = min(window_size, rh->recvWnd);
                empty = CreateSemaphore(NULL, last_released, senderWindow, NULL);
                full = CreateSemaphore(NULL, 0, senderWindow, NULL);

                int event_select = WSAEventSelect(sock, data_received_event, FD_READ | FD_CLOSE);
                if (event_select == SOCKET_ERROR) {
                    printf("event failed with %d\n", WSAGetLastError());
                    return -1;
                }

                return STATUS_OK;
            }
        }
    }
    return TIMEOUT;
}
int SenderSocket::Send(char*buf, int bytes)
{
    if (!connection_open) return NOT_CONNECTED;

    if (exception_exit)
    {
        SetEvent(eventQuit);
        WaitForSingleObject(stats_thread_handle, INFINITE);
        CloseHandle(stats_thread_handle);
        SetEvent(eventQuit);
        WaitForSingleObject(worker_thread_handle, INFINITE);
        CloseHandle(worker_thread_handle);
        return FAILED_SEND;
    }

    HANDLE events[] = { empty };
    WaitForMultipleObjects(1, events, false, INFINITE);

    Packet* packet = new Packet();
    PacketData* packet_data = new PacketData();

    packet_data->sdh.seq = current_seq;
    memcpy(packet_data->data, buf, bytes);

    int packet_position = current_seq % window_size;
    packet->size = sizeof(SenderDataHeader) + bytes;
    packet->pd = packet_data;

    memcpy(packets_buffer + packet_position, packet, sizeof(Packet));
    current_seq++;

    ReleaseSemaphore(full, 1, NULL);

    return STATUS_OK;

}


int SenderSocket::Close(int senderWindow, LinkProperties* lp)
{
    if (!connection_open) return NOT_CONNECTED;

    SetEvent(eventQuit);
    WaitForSingleObject(stats_thread_handle, INFINITE);
    CloseHandle(stats_thread_handle);
    SetEvent(eventQuit);
    WaitForSingleObject(worker_thread_handle, INFINITE);
    CloseHandle(worker_thread_handle);

    end_data_time = clock();

    SenderSynHeader* ssh = new SenderSynHeader();

    ssh->sdh.flags.reserved = 0;
    ssh->sdh.flags.SYN = 0;
    ssh->sdh.flags.FIN = 1;
    ssh->sdh.flags.ACK = 0;
    ssh->sdh.seq = current_ack;

    ssh->lp = *lp;
    ssh->lp.bufferSize = senderWindow + MAX_ATTEMPTS;

    fin_start_time = clock() - start_time;
    for (int i = 0; i < MAX_ATTEMPTS; i++) {
        current_time = clock() - start_time;
        //printf("[%.3f] --> FIN 0 (attempt %d of %d, RTO %.3f) \n", (float)(current_time / (float)1000), i + 1, MAX_ATTEMPTS, rto);

        if (sendto(sock, (char*)ssh, sizeof(SenderSynHeader), 0, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
        {
            printf("failed sendto with %d\n", WSAGetLastError());
            return FAILED_SEND;
        };

        struct sockaddr_in res_server;
        int res_server_size = sizeof(res_server);
        char* res_buf = new char[sizeof(ReceiverHeader)];

        int available = WaitForSingleObject(data_received_event, rto*1000);

        if (available == WAIT_TIMEOUT) {
            continue;
        }

        if (available == SOCKET_ERROR)
        {
            printf("failed recvfrom with %d\n", WSAGetLastError());
            return FAILED_RECV;
        }; 

        int bytes_received = recvfrom(sock, res_buf, sizeof(ReceiverHeader), 0, (struct sockaddr*)&res_server, &res_server_size);

        if (bytes_received == SOCKET_ERROR)
        {
            printf("failed recvfrom with %d\n", WSAGetLastError());
            return FAILED_RECV;
        };

        ReceiverHeader* rh = (ReceiverHeader*)res_buf;

        if (rh->flags.FIN == 1 && rh->flags.ACK == 1) {
            current_time = clock() - start_time;
            fin_end_time = current_time;
            received_checksum = rh->recvWnd;
            current_ack = rh->ackSeq;
            printf("[%.3f] <-- FIN-ACK %d window %X\n", (float)(current_time / (float)1000), rh->ackSeq, rh->recvWnd);
            connection_open = false;
            return STATUS_OK;
        }
        
    }
    return TIMEOUT;
}