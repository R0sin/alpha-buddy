#pragma once

#include <cstddef>
#include <cstdint>

namespace alpha_buddy {
namespace ptpip {

constexpr uint16_t kDefaultPort = 15740;
constexpr uint32_t kProtocolVersion = 0x00010000;

enum class PacketType : uint32_t {
    InitCommandRequest = 0x0001,
    InitCommandAck = 0x0002,
    InitEventRequest = 0x0003,
    InitEventAck = 0x0004,
    InitFailed = 0x0005,
    OperationRequest = 0x0006,
    OperationResponse = 0x0007,
    Event = 0x0008,
    StartData = 0x0009,
    Data = 0x000A,
    CancelData = 0x000B,
    EndData = 0x000C,
    ProbeRequest = 0x000D,
    ProbeResponse = 0x000E,
};

enum class DataPhaseInfo : uint32_t {
    Unknown = 0,
    NoDataOrDataIn = 1,
    DataOut = 2,
};

struct PacketHeader {
    uint32_t length = 0;
    PacketType type = PacketType::InitFailed;
};

bool parseHeader(const uint8_t* data, size_t length, PacketHeader& output);
bool parseInitCommandAck(const uint8_t* data,
                         size_t length,
                         uint32_t& connectionId,
                         char* cameraName,
                         size_t cameraNameCapacity);
bool parseOperationResponse(const uint8_t* data, size_t length, uint16_t& responseCode);
bool parseStartData(const uint8_t* data,
                    size_t length,
                    uint32_t& transactionId,
                    uint64_t& expectedBytes);

size_t buildInitCommandRequest(const uint8_t guid[16],
                               const char* friendlyName,
                               uint8_t* output,
                               size_t capacity);
size_t buildInitEventRequest(uint32_t connectionId, uint8_t* output, size_t capacity);
size_t buildOperationRequest(uint16_t operationCode,
                             uint32_t transactionId,
                             DataPhaseInfo dataPhase,
                             const uint32_t* parameters,
                             size_t parameterCount,
                             uint8_t* output,
                             size_t capacity);
size_t buildStartDataPacket(uint32_t transactionId,
                            uint64_t payloadBytes,
                            uint8_t* output,
                            size_t capacity);
size_t buildDataPacket(uint32_t transactionId,
                       const uint8_t* payload,
                       size_t payloadBytes,
                       uint8_t* output,
                       size_t capacity);
size_t buildEndDataPacket(uint32_t transactionId,
                          uint8_t* output,
                          size_t capacity);
size_t buildProbeResponse(uint8_t* output, size_t capacity);

uint16_t readLe16(const uint8_t* data);
uint32_t readLe32(const uint8_t* data);
uint64_t readLe64(const uint8_t* data);
void writeLe16(uint8_t* data, uint16_t value);
void writeLe32(uint8_t* data, uint32_t value);
void writeLe64(uint8_t* data, uint64_t value);

}  // namespace ptpip
}  // namespace alpha_buddy
