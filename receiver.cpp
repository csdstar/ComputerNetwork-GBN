#include "proto.h"

using namespace std;

// 发送ACK的函数,组装ack，序列化并发送
void sendACK(SOCKET sock, int ackSeqNo, sockaddr_in &senderAddr, int senderLen)
{
    PDU ack;
    ack.totalPackets = 0; // ACK 不携带数据
    ack.seqNo = ackSeqNo; // 要确认的序列号
    ack.length = 0;
    ack.data = nullptr;
    ack.checksum = 0;

    int outLen;
    char *ackBuffer = serializePDU(ack, outLen);
    sendto(sock, ackBuffer, outLen, 0, (sockaddr *)&senderAddr, senderLen);
    delete[] ackBuffer;
}

int main()
{
    // 加载配置文件
    auto config = loadConfig("config.cfg");
    int port = stoi(config["UDPPort"]);
    int dataSize = stoi(config["DataSize"]);
    int errorRate = stoi(config["ErrorRate"]);
    int lostRate = stoi(config["LostRate"]);
    int swSize = stoi(config["SWSize"]);
    int initSeq = stoi(config["InitSeqNo"]);
    int timeout = stoi(config["Timeout"]);


    string sendLogPath = config["SendLogPath"];
    string recvLogPath = config["RecvLogPath"];
    string inputPath = config["InputPath"];
    string outputPath = config["OutputPath"];


    // 初始化Winsock环境
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }

    // 创建一个UDP socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        std::cerr << "Socket creation failed.\n";
        WSACleanup();
        return 1;
    }

    // 设置本机地址结构
    sockaddr_in localAddr = {};
    localAddr.sin_family = AF_INET;         // 使用IPv4
    localAddr.sin_port = htons(port);       // 设置接收端口
    localAddr.sin_addr.s_addr = INADDR_ANY; // 设置本机IP地址

    if (bind(sock, (sockaddr *)&localAddr, sizeof(localAddr)) == SOCKET_ERROR)
    {
        std::cerr << "Bind failed.\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    cout << "Initialize success, waiting for data...\n\n";

    int seq = initSeq;        // 初始化待接收的序列号
    int expectedPackets = -1; // 预期接收的包数

    // 用于存储接收到的包（按序号）
    map<int, vector<char>> receivedData;

    // 记录每个包的重传次数，初始化为0
    unordered_map<int, int> receiveCount;

    // 打开日志文件
    ofstream log(recvLogPath);
    if (!log.is_open())
    {
        cerr << "can't open receiver_log" << endl;
        return 1;
    }

    while (1)
    {
        char recvBuf[65536]; // 最大允许的数据缓冲区（64KB，可根据协议调小）
        sockaddr_in senderAddr;
        socklen_t senderLen = sizeof(senderAddr);

        // 接收数据（recvBuf 为原始字节流）
        int ret = recvfrom(sock, recvBuf, sizeof(recvBuf), 0, (sockaddr *)&senderAddr, &senderLen);

        // 接收失败
        if (ret == SOCKET_ERROR)
        {
            cerr << "recvfrom failed.\n";
            return 1;
        }

        // 将接受的数据反序列化为 PDU
        PDU packet = deserializePDU(recvBuf, ret);
        bool isValid = packet.isValid(); // 检查数据包的有效性

        // 记录当前包的接收次数
        if (receiveCount.count(packet.seqNo) == 0)
            receiveCount[packet.seqNo] = 0;

        int count = ++receiveCount[packet.seqNo];
            

        // 若接收帧正确，且就是当前的等待帧
        if (isValid && packet.seqNo == seq)
        {
            // 第一个包到达时，记录总包数
            if (seq == initSeq)
                expectedPackets = packet.totalPackets;

            printProgressBar(seq - initSeq + 1, expectedPackets);
            logRecv(log, count, seq, packet.seqNo, "OK");

            // 更新下一个期望的序列号
            ++seq;

            // 存储数据
            receivedData[packet.seqNo - initSeq] = vector<char>(packet.data, packet.data + packet.length);

            // 发送 ACK 确认包
            sendACK(sock, packet.seqNo, senderAddr, senderLen);

            // 若最后一个包确认收到，退出循环
            if (expectedPackets > 0 && packet.seqNo == expectedPackets + initSeq - 1)
                break;
        }

        // 接收包无效
        else if (!isValid)
        {
            // cerr << "Invalid packet received, seqNo: " << packet.seqNo << endl;
            logRecv(log, count, seq, packet.seqNo, "DataErr");

            // 重新发送先前的 ACK 确认包
            sendACK(sock, seq - 1, senderAddr, senderLen);
        }

        // 接收包有效但不是当前等待帧，丢弃收到的数据包
        else
        {
            // cerr << "not the right packet, " << packet.seqNo << " != " << seq << endl;
            logRecv(log, count, seq, packet.seqNo, "NoErr");

            // 重新发送先前的 ACK 确认包
            sendACK(sock, seq - 1, senderAddr, senderLen);
        }
    }

    // 开始拼接文件
    ofstream outfile(outputPath, ios::binary);
    for (int i = 0; i < expectedPackets; ++i)
    {
        outfile.write(receivedData[i].data(), receivedData[i].size());
    }
    outfile.close();
    cout << "\n\nFile received and reconstructed successfully.\n";

    closesocket(sock);
    WSACleanup();
    system("pause");

    return 0;
}