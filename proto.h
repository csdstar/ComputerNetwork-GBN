#include <iostream>
#include <cstdio>
#include <map>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <winsock2.h> // Windows下网络编程核心头文件
#include <ws2tcpip.h> // 包含 inet_pton, getaddrinfo 等函数
#pragma comment(lib, "ws2_32.lib") // 链接 Winsock 库

using namespace std;

// 读取配置文件，返回一个键值对的 map
map<string, string> loadConfig(const string& filename) {
    map<string, string> config;
    ifstream file(filename);
    string line;
    while (getline(file, line)) {
        // 去除注释和空行
        if (line.empty() || line[0] == '#') continue;

        istringstream iss(line);
        string key, value;
        if (getline(iss, key, '=') && getline(iss, value)) {
            config[key] = value;
        }
    }
    return config;
}

// 实现 CRC-CCITT 校验算法，用于检测数据完整性
uint16_t crc16(const char *data, size_t length)
{
    // CRC-CCITT 校验算法，用0x1021作为多项式
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= (uint8_t)data[i] << 8;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

// 测试函数，用于打印buffer
void printBufferHex(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        printf("%02X ", (unsigned char)data[i]);
        if ((i+1) % 16 == 0) printf("\n");
    }
    printf("\n");
}


// 定义 PDU（协议数据单元）结构体
#pragma pack(1) // 结构体紧凑对齐
struct PDU
{
    int32_t totalPackets;  // 包总数
    uint32_t seqNo;    // 序号
    uint16_t length;   // 数据部分的长度（不超过4096字节）
    char* data;        // 数据部分的内容
    uint16_t checksum; // CRC 校验

    void allocateData(int len) {
        length = len;
        data = new char[len];
    }

    // 进行CRC校验计算
    void calculateChecksum() {
        vector<char> buffer(sizeof(totalPackets) + sizeof(seqNo) + sizeof(length) + length);

        memcpy(buffer.data(), &totalPackets, sizeof(totalPackets));
        memcpy(buffer.data() + sizeof(totalPackets), &seqNo, sizeof(seqNo));
        memcpy(buffer.data() + sizeof(totalPackets) + sizeof(seqNo), &length, sizeof(length));
        memcpy(buffer.data() + sizeof(totalPackets) + sizeof(seqNo) + sizeof(length), data, length);

        // printBufferHex(buffer.data(), buffer.size());
        this->checksum = crc16(buffer.data(), buffer.size());
    }

    // 检测校验码
    bool isValid() const {
        if(data == nullptr && checksum == 0)
            return true;

        vector<char> buffer(sizeof(totalPackets) + sizeof(seqNo) + sizeof(length) + length);
        size_t headerSize = sizeof(int) + sizeof(uint32_t) + sizeof(uint16_t);

        memcpy(buffer.data(), &totalPackets, sizeof(totalPackets));
        memcpy(buffer.data() + sizeof(totalPackets), &seqNo, sizeof(seqNo));
        memcpy(buffer.data() + sizeof(totalPackets) + sizeof(seqNo), &length, sizeof(length));
        memcpy(buffer.data() + sizeof(totalPackets) + sizeof(seqNo) + sizeof(length), data, length);

        uint16_t calc = crc16(buffer.data(), buffer.size());

        return checksum == calc;
    }

    // 拷贝构造函数（深拷贝）
    PDU(const PDU& other) {
        totalPackets = other.totalPackets;
        seqNo = other.seqNo;
        length = other.length;
        checksum = other.checksum;

        if (length > 0 && other.data) {
            data = new char[length];
            memcpy(data, other.data, length);
        } else {
            data = nullptr;
        }
    }

    // 拷贝赋值运算符（深拷贝）
    PDU& operator=(const PDU& other) {
        if (this != &other) {
            delete[] data;

            totalPackets = other.totalPackets;
            seqNo = other.seqNo;
            length = other.length;
            checksum = other.checksum;

            if (length > 0 && other.data) {
                data = new char[length];
                memcpy(data, other.data, length);
            } else {
                data = nullptr;
            }
        }
        return *this;
    }

    // 构造和析构函数
    PDU() = default;
    ~PDU() {
        delete[] data;
    }
};

// 将 PDU 转换为连续内存块，计算长度并赋值给 outLen
char* serializePDU(const PDU& pdu, int& outLen) {
    outLen = sizeof(int) + sizeof(uint32_t) + sizeof(uint16_t) + pdu.length + sizeof(uint16_t);
    char* buffer = new char[outLen];

    int offset = 0;

    // 序列化 totalPackets
    memcpy(buffer + offset, &pdu.totalPackets, sizeof(int32_t));
    offset += sizeof(pdu.totalPackets);

    // 序列化 seqNo
    memcpy(buffer + offset, &pdu.seqNo, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // 序列化 length
    memcpy(buffer + offset, &pdu.length, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    // 只有当 length 大于 0 时，才会序列化 data
    if (pdu.length > 0) {
        memcpy(buffer + offset, pdu.data, pdu.length);
        offset += pdu.length;
    }

    // 序列化 checksum
    memcpy(buffer + offset, &pdu.checksum, sizeof(uint16_t));

    return buffer;
}

// 从接收到的 buffer 中还原出一个 PDU 实例
PDU deserializePDU(const char* buffer, int bufferLen) {
    PDU pdu;
    int offset = 0;

    memcpy(&pdu.totalPackets, buffer + offset, sizeof(int32_t));
    offset += sizeof(int32_t);

    memcpy(&pdu.seqNo, buffer + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    memcpy(&pdu.length, buffer + offset, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    if (pdu.length > 0) {
        pdu.data = new char[pdu.length];
        memcpy(pdu.data, buffer + offset, pdu.length);
        offset += pdu.length;
    } else {
        pdu.data = nullptr; // 或者赋值为空，表示没有数据
    }        

    memcpy(&pdu.checksum, buffer + offset, sizeof(uint16_t));

    return pdu;
}


// 发送方日志函数
void logSend(
    ofstream& log,        // 日志文件流
    int sendCount,        // 发送计数器
    uint32_t seqNo,       // 当前发送的序列号
    const string& status, // 发送状态（NEW/TO/RT）
    uint32_t ackedNo      // 已接收的ACK序列号
) {
    // 获取当前时间
    auto now = chrono::system_clock::now();
    time_t now_c = chrono::system_clock::to_time_t(now);

    // 写入时间戳
    log << put_time(localtime(&now_c), "[%Y-%m-%d %H:%M:%S]  ");

    // 写入日志内容
    log << sendCount << ", pdu_to_send=" << seqNo << ", status=" << status << ", ackedNo=" << ackedNo << endl;
}

// 接收方日志函数
void logRecv(
    ofstream& log,          // 日志文件流
    int recvCount,          // 接收计数器
    uint32_t expectedSeqNo, // 期望的序列号
    uint32_t receivedSeqNo, // 实际接收的序列号
    const string& status    // 接收状态（OK/DataErr/NoErr）
) {
    // 获取当前时间
    auto now = chrono::system_clock::now();
    time_t now_c = chrono::system_clock::to_time_t(now);

    // 写入时间戳
    log << put_time(localtime(&now_c), "[%Y-%m-%d %H:%M:%S]  ");

    // 写入日志内容
    log << recvCount << ", pdu_exp=" << expectedSeqNo << ", pdu_recv=" << receivedSeqNo << ", status=" << status << endl;
}

// 简单的进度条函数，显示当前接收进度
void printProgressBar(int current, int total) {
    int barWidth = 50;
    float progress = static_cast<float>(current) / total;
    int pos = static_cast<int>(barWidth * progress);

    cout << "\r[";
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos)
            cout << "#";
        else
            cout << ".";
    }
    cout << "] " << current << "/" << total << " (" << static_cast<int>(progress * 100) << "%)" << flush;
}