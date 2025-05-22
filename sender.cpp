#include <thread>
#include <random>
#include <cmath>
#include "proto.h"

// 发送PDU函数，能随机模拟丢包或注入错误
void sendWithError(
    int sock, const sockaddr_in &destAddr, PDU &pdu, // socket及PDU参数
    int &sendCount,                                  // 重传计数器
    const string &status,                            // 发送状态
    int lostRate, int errorRate,                     // 丢包率和错误率
    int ackedNo,                                     // 已接收的ACK序列号
    ofstream &log                                    // 日志文件
)
{
    // 随机数生成器
    static random_device rd;
    static mt19937 gen(rd());
    static uniform_int_distribution<> dist(0, 99);

    // 生成一个 0-99 的随机值来模拟丢包或注入错误的情况
    int randVal = dist(gen);

    // 序列化 PDU
    int packetLen;
    char *serialized = serializePDU(pdu, packetLen);

    // 根据随机值判断是否丢包、注入错误或正常发送
    if (randVal < lostRate)
    {
        // 丢弃，不发送，仅写入发送日志
        logSend(log, sendCount, pdu.seqNo, status, ackedNo);
    }
    else if (randVal < lostRate + errorRate)
    {
        // 注入错误，不重新计算 checksum，故意让校验失败
        serialized[sizeof(int) + sizeof(uint32_t) + sizeof(uint16_t)] ^= 0xFF; // 反转数据部分第一个字节
        sendto(sock, serialized, packetLen, 0, (sockaddr *)&destAddr, sizeof(destAddr));
        logSend(log, sendCount, pdu.seqNo, status, ackedNo);
    }
    else
    {
        // 正常发送
        sendto(sock, serialized, packetLen, 0, (sockaddr *)&destAddr, sizeof(destAddr));
        logSend(log, sendCount, pdu.seqNo, status, ackedNo);
    }

    // 释放序列化后的内存
    delete[] serialized;
}

// 读取文件并切分为多个 PDU
vector<PDU> splitFileToPackets(const string &filename, int dataSize, int initSeq, int &totalPackets)
{
    cout << "split file" << endl;
    ifstream file(filename, ios::binary | ios::ate);
    if (!file.is_open())
    {
        cerr << "Failed to open file: " << filename << endl;
        exit(1);
    }

    streamsize fileSize = file.tellg(); // 获取文件大小
    file.seekg(0, ios::beg);            // 回到文件开头

    totalPackets = static_cast<int>(ceil((double)fileSize / dataSize));
    vector<PDU> packets;
    packets.reserve(totalPackets);

    for (int i = initSeq; i < totalPackets + initSeq; ++i)
    {
        int index = i - initSeq; // 相对索引（0开始）

        // 每个包的大小, 考虑最后一个包需要额外切分
        int thisSize = (index < totalPackets - 1)
                           ? dataSize
                           : static_cast<int>(fileSize - (totalPackets - 1) * dataSize);

        PDU pdu;
        pdu.totalPackets = totalPackets; // 设置总包数
        pdu.seqNo = i;
        pdu.length = thisSize;
        pdu.allocateData(thisSize);    // 分配数据空间
        file.read(pdu.data, thisSize); // 读取对应内容
        pdu.calculateChecksum();       // 计算校验和

        packets.push_back(pdu);
    }

    file.close();
    return packets;
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
    ofstream log(sendLogPath);
    if (!log.is_open())
    {
        cerr << "can't open sender_log" << endl;
        return 1;
    }

    // 参数设置
    int totalPackets = 0;                                                                 // 总包数
    vector<PDU> packets = splitFileToPackets(inputPath, dataSize, initSeq, totalPackets); // 从文件中切分数据包

    int seq = initSeq;        // 当前窗口左侧序号
    int nextSeqNum = initSeq; // 下一个要发送的包序列号

    unordered_map<int, int> retransmitCount; // 记录每个包的重传次数，初始化为0
    for (int i = 0; i < totalPackets; ++i)
    {
        retransmitCount[initSeq + i] = 0;
    }

    cout << "totalPackets: " << totalPackets << endl;
    cout << "Initialize success, preparing to send...\n\n";
    printProgressBar(0, totalPackets);                           // 打印初始进度条
    auto senderStartTime = chrono::high_resolution_clock::now(); // 记录发送开始时间
    int TOCount = 0;                                             // 记录超时重发次数
    int RTCount = 0;                                             // 记录丢包/错包重传次数
    int totalSendCount = 0;                                      // 记录总发送次数

    // ACK接收缓冲区
    char recvBuf[65536];      // 最大允许的数据缓冲区（64KB，可根据协议调小）
    int ackReceived = -1;     // 已收到的最远ACK序列号
    bool timeoutFlag = false; // 超时标志

    // 发送窗口内的数据包
    while (seq < totalPackets + initSeq)
    {
        // 当下一个要发送的包还在窗口内时发送数据包
        while (nextSeqNum < seq + swSize && nextSeqNum < totalPackets + initSeq)
        {
            // 从切分好的列表中获取当前要发送的包
            PDU &pdu = packets[nextSeqNum - initSeq];

            // 查map获取当前包的重传次数
            int sendCount = retransmitCount[nextSeqNum];
            retransmitCount[nextSeqNum] = ++sendCount; // 更新重传次数

            // 定义当前包的发送状态：初次发送/超时/重传
            string status = sendCount == 1 ? "NEW" : (timeoutFlag ? "TO " : "RT ");

            // 重传计数
            if (status == "TO ")
                TOCount++;
            else if (status == "RT ")
                RTCount++;

            // 有出错概率地发送数据包
            sendWithError(sock, destAddr, pdu, sendCount, status, lostRate, errorRate, ackReceived, log);

            totalSendCount++;    // 统计总发送次数
            nextSeqNum++;        // 更新下一个序列号
            timeoutFlag = false; // 重置timeout标志
        }

        // 将窗口内数据包全部发送完毕，开始等待ACK
        bool ackFlag = false;                                  // 当前超时期内是否收到ACK的标志
        auto startTime = chrono::high_resolution_clock::now(); // 记录开始等待时间

        // 等待ACK确认
        while (!ackFlag)
        {
            // 检查超时
            auto currentTime = chrono::high_resolution_clock::now();
            auto elapsed = chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);

            // 超时处理：更新窗口位置，并发送窗口内所有未确认包
            if (elapsed.count() >= timeout)
            {
                timeoutFlag = true;                  // 设置超时标志
                seq = max(ackReceived + 1, initSeq); // 更新窗口起始位置
                nextSeqNum = seq;                    // 重置下一个要发送的包序列号
                break;
            }

            // 未超时，正常接收 ACK
            int ret = recvfrom(sock, recvBuf, sizeof(recvBuf), 0, (sockaddr *)&destAddr, &receiverLen);
            if (ret > 0)
            {
                PDU ack = deserializePDU(recvBuf, ret);

                // 若收到ACK，则更新窗口
                if (ack.isValid())
                {
                    ackFlag = true;                                            // 收到有效的ACK
                    ackReceived = max(int(ack.seqNo), ackReceived);            // 更新已收到的最新ACK序列号
                    seq = ackReceived + 1;                                     // 更新窗口的起始位置
                    printProgressBar(ackReceived - initSeq + 1, totalPackets); // 打印进度条
                    break;
                }
                else
                    continue; // 无效的ACK，继续等待
            }
        }

        // 全部包已确认，退出循环，打印统计信息
        if (ackReceived >= totalPackets + initSeq - 1)
        {
            cout << "All packets acknowledged, exiting...\n";

            auto senderEndTime = chrono::high_resolution_clock::now();
            auto duration = chrono::duration_cast<chrono::seconds>(senderEndTime - senderStartTime).count();
            cout << "\n[INFO] Total transmission time: " << duration << " s" << endl << endl;

            cout << "Total packets sent: " << totalSendCount << endl;
            
            cout << "Total Retransmissions: " << TOCount + RTCount << endl;
            
            cout << "Average Retransmissions: " << fixed << setprecision(2) << (double)(TOCount + RTCount) / totalPackets << endl;

            cout << "Timeout Retransmissions: " << TOCount << " / " << totalSendCount << " = " << fixed << setprecision(2) << (double)TOCount / totalSendCount * 100 << "%" << endl;

            cout << "Error or Lost Retransmissions: " << RTCount << " / " << totalSendCount<< " = " << fixed << setprecision(2) << (double)RTCount / totalSendCount * 100 << "%" << endl;
            
            break; // 退出循环
        }
    }

    // 关闭socket并清理Winsock环境
    log.close();
    closesocket(sock);
    WSACleanup();
    system("pause"); // 暂停，等待用户输入
    return 0;
}
