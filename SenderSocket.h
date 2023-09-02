#pragma once
// SenderSocket.h
#define MAGIC_PORT 22345 // receiver listens on this port
#define MAX_PKT_SIZE (1500-28) // maximum UDP packet size accepted by receiver
// possible status codes from ss.Open, ss.Send, ss.Close
#define STATUS_OK 0 // no error
#define ALREADY_CONNECTED 1 // second call to ss.Open() without closing connection
#define NOT_CONNECTED 2 // call to ss.Send()/Close() without ss.Open()
#define INVALID_NAME 3 // ss.Open() with targetHost that has no DNS entry
#define FAILED_SEND 4 // sendto() failed in kernel
#define TIMEOUT 5 // timeout after all retx attempts are exhausted
#define FAILED_RECV 6 // recvfrom() failed in kernel
#define MAGIC_PROTOCOL 0x8311AA 

#define FORWARD_PATH 0
#define RETURN_PATH 1 

#define MAX_SYN_ATTEMPTS 3
#define MAX_ATTEMPTS 5
#define MAX_DATA_ATTEMPTS 50

#define ALPHA 0.125
#define BETA 0.25

#pragma pack(push,1) 
class Flags {
public:
    DWORD reserved : 5; // must be zero
    DWORD SYN : 1;
    DWORD ACK : 1;
    DWORD FIN : 1;
    DWORD magic : 24;
    Flags() { memset(this, 0, sizeof(*this)); magic = MAGIC_PROTOCOL; }
};
class SenderDataHeader {
public:
    Flags flags;
    DWORD seq; // must begin from 0
};
class PacketData {
public:
    SenderDataHeader sdh;
    char data[MAX_PKT_SIZE];
};
class Packet {
public:
    int size;
    clock_t txTime;
    PacketData* pd;
};
class LinkProperties {
public:
    // transfer parameters
    float RTT; // propagation RTT (in sec)
    float speed; // bottleneck bandwidth (in bits/sec)
    float pLoss[2]; // probability of loss in each direction
    DWORD bufferSize; // buffer size of emulated routers (in packets)
    LinkProperties() { memset(this, 0, sizeof(*this)); }
};
class SenderSynHeader {
public:
    SenderDataHeader sdh;
    LinkProperties lp;
};
class ReceiverHeader {
public:
    Flags flags;
    DWORD recvWnd; // receiver window for flow control (in pkts)
    DWORD ackSeq; // ack value = next expected sequence
};
#pragma pack(pop)

class SenderSocket {
public:
    SOCKET sock;
    struct sockaddr_in server;
    bool connection_open;
    int current_seq, current_ack, timed_out_packets, current_base;
    long long int bytes_acked;
    HANDLE	eventQuit;
    DWORD received_checksum;
    double rto, estimated_rtt, dev_rtt;
    double start_data_time, end_data_time, average_rate;
    clock_t start_time, current_time, syn_start_time, syn_end_time, fin_start_time, fin_end_time;
    HANDLE stats_thread_handle, worker_thread_handle;
    HANDLE full, empty, data_received_event;
    Packet* packets_buffer;
    int base, window_size, retry_count, duplicate_ack, fast_retransmit, last_released, effective_window;
    bool close_called, exception_exit;

    SenderSocket();
    ~SenderSocket();
    int Open(char* host, int port, int senderWindow, LinkProperties* lp);
    int Send(char* buf, int bytes);
    int Close(int senderWindow, LinkProperties* lp);
    int sendData(int pkt_no);
    int receiveData(bool& retransmitted);

    static UINT stats_thread(LPVOID pParam);
    static UINT worker_thread(LPVOID pParam);
};