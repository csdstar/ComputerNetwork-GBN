#include <thread>
#include <random>
#include <cmath>
#include "proto.h"

// 单元测试
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
        WSACleanup(); // 清理Winsock环境
        return 1;
    }

    // 设置socket为非阻塞的
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    // 设置目标地址结构
    sockaddr_in destAddr = {};
    destAddr.sin_family = AF_INET;                     // 使用IPv4
    destAddr.sin_port = htons(port);                   // 设置目标端口
    destAddr.sin_addr.s_addr = inet_addr("127.0.0.1"); // 设置目标IP地址，这里使用本机地址进行测试

    socklen_t receiverLen = sizeof(destAddr); // 设置接收地址长度

    // 打开日志文件
    ofstream log("./log/test_sender_log.txt");
    if (!log.is_open())
    {
        cerr << "can't open sender_log" << endl;
        return 1;
    }

    // 参数设置
    int totalPackets = 10;

    // 手动生成并组装10个PDU进行测试
    for (int i = 0; i < totalPackets; ++i)
    {
        PDU pdu;
        pdu.totalPackets = totalPackets; // 设置总包数
        pdu.seqNo = initSeq + i;
        pdu.length = dataSize;                    // 设置数据长度
        pdu.allocateData(dataSize);               // 分配数据空间
        memset(pdu.data, 'A' + i % 26, dataSize); // 设置数据区内容
        pdu.calculateChecksum();                  // 计算校验和

        // 序列化 PDU
        int packetLen;
        char *serialized = serializePDU(pdu, packetLen);

        sendto(sock, serialized, packetLen, 0, (sockaddr *)&destAddr, sizeof(destAddr));
        delete[] serialized; // 释放序列化后的数据
    }

    // 关闭socket并清理Winsock环境
    log.close();
    closesocket(sock);
    WSACleanup();
    system("pause"); // 暂停，等待用户输入
    return 0;
}