#include "alpha_buddy/PtpIpProtocol.h"

#include <cstring>

namespace alpha_buddy {
namespace ptpip {

// Reduced from CIPA DC-005-2005 and Alpha-Fairy's PtpIpCamera implementation
// at commit fd1f2989f952a77d01cdb23b788058c39f091c89.

namespace {

constexpr size_t kHeaderBytes = 8;
constexpr size_t kGuidBytes = 16;
constexpr size_t kMaxFriendlyNameChars = 120;

size_t writeHeader(PacketType type, uint32_t length, uint8_t* output, size_t capacity) {
    if (output == nullptr || capacity < kHeaderBytes || length > capacity) {
        return 0;
    }
    writeLe32(output, length);
    writeLe32(output + 4, static_cast<uint32_t>(type));
    return kHeaderBytes;
}

}  // namespace

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) |
           (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t readLe64(const uint8_t* data) {
    return static_cast<uint64_t>(readLe32(data)) |
           (static_cast<uint64_t>(readLe32(data + 4)) << 32U);
}

void writeLe16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value & 0xFFU);
    data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void writeLe32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value & 0xFFU);
    data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    data[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    data[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

void writeLe64(uint8_t* data, uint64_t value) {
    writeLe32(data, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
    writeLe32(data + 4, static_cast<uint32_t>(value >> 32U));
}

bool parseHeader(const uint8_t* data, size_t length, PacketHeader& output) {
    if (data == nullptr || length < kHeaderBytes) {
        return false;
    }
    output.length = readLe32(data);
    output.type = static_cast<PacketType>(readLe32(data + 4));
    return output.length >= kHeaderBytes;
}

bool parseInitCommandAck(const uint8_t* data,
                         size_t length,
                         uint32_t& connectionId,
                         char* cameraName,
                         size_t cameraNameCapacity) {
    PacketHeader header;
    if (!parseHeader(data, length, header) ||
        header.type != PacketType::InitCommandAck ||
        header.length > length ||
        header.length < 32 ||
        cameraName == nullptr ||
        cameraNameCapacity == 0) {
        return false;
    }

    connectionId = readLe32(data + 8);
    const size_t nameStart = 28;
    const size_t nameEnd = header.length >= 4 ? header.length - 4 : header.length;
    size_t outputIndex = 0;
    for (size_t index = nameStart;
         index + 1 < nameEnd && outputIndex + 1 < cameraNameCapacity;
         index += 2) {
        const char value = static_cast<char>(data[index]);
        if (value == '\0') {
            break;
        }
        cameraName[outputIndex++] = value;
    }
    cameraName[outputIndex] = '\0';
    return true;
}

bool parseOperationResponse(const uint8_t* data, size_t length, uint16_t& responseCode) {
    PacketHeader header;
    if (!parseHeader(data, length, header) ||
        header.type != PacketType::OperationResponse ||
        header.length > length ||
        header.length < 10) {
        return false;
    }
    responseCode = readLe16(data + 8);
    return true;
}

bool parseStartData(const uint8_t* data,
                    size_t length,
                    uint32_t& transactionId,
                    uint64_t& expectedBytes) {
    PacketHeader header;
    if (!parseHeader(data, length, header) ||
        header.type != PacketType::StartData ||
        header.length > length ||
        header.length < 20) {
        return false;
    }
    transactionId = readLe32(data + 8);
    expectedBytes = readLe64(data + 12);
    return true;
}

size_t buildInitCommandRequest(const uint8_t guid[16],
                               const char* friendlyName,
                               uint8_t* output,
                               size_t capacity) {
    if (guid == nullptr || friendlyName == nullptr || output == nullptr) {
        return 0;
    }
    const size_t nameChars = std::strlen(friendlyName);
    if (nameChars > kMaxFriendlyNameChars) {
        return 0;
    }
    const size_t nameBytes = (nameChars + 1) * 2;
    const size_t packetBytes = kHeaderBytes + kGuidBytes + nameBytes + sizeof(uint32_t);
    if (writeHeader(PacketType::InitCommandRequest,
                    static_cast<uint32_t>(packetBytes),
                    output,
                    capacity) == 0) {
        return 0;
    }
    std::memcpy(output + kHeaderBytes, guid, kGuidBytes);
    size_t cursor = kHeaderBytes + kGuidBytes;
    for (size_t index = 0; index < nameChars; ++index) {
        output[cursor++] = static_cast<uint8_t>(friendlyName[index]);
        output[cursor++] = 0;
    }
    output[cursor++] = 0;
    output[cursor++] = 0;
    writeLe32(output + cursor, kProtocolVersion);
    return packetBytes;
}

size_t buildInitEventRequest(uint32_t connectionId, uint8_t* output, size_t capacity) {
    constexpr size_t packetBytes = 12;
    if (writeHeader(PacketType::InitEventRequest, packetBytes, output, capacity) == 0) {
        return 0;
    }
    writeLe32(output + 8, connectionId);
    return packetBytes;
}

size_t buildOperationRequest(uint16_t operationCode,
                             uint32_t transactionId,
                             DataPhaseInfo dataPhase,
                             const uint32_t* parameters,
                             size_t parameterCount,
                             uint8_t* output,
                             size_t capacity) {
    constexpr size_t fixedBytes = 18;
    constexpr size_t maxParameters = 5;
    if (output == nullptr || parameterCount > maxParameters ||
        (parameterCount > 0 && parameters == nullptr)) {
        return 0;
    }
    const size_t packetBytes = fixedBytes + parameterCount * sizeof(uint32_t);
    if (writeHeader(PacketType::OperationRequest,
                    static_cast<uint32_t>(packetBytes),
                    output,
                    capacity) == 0) {
        return 0;
    }
    writeLe32(output + 8, static_cast<uint32_t>(dataPhase));
    writeLe16(output + 12, operationCode);
    writeLe32(output + 14, transactionId);
    for (size_t index = 0; index < parameterCount; ++index) {
        writeLe32(output + fixedBytes + index * sizeof(uint32_t), parameters[index]);
    }
    return packetBytes;
}

size_t buildStartDataPacket(uint32_t transactionId,
                            uint64_t payloadBytes,
                            uint8_t* output,
                            size_t capacity) {
    constexpr size_t packetBytes = 20;
    if (writeHeader(PacketType::StartData, packetBytes, output, capacity) == 0) {
        return 0;
    }
    writeLe32(output + 8, transactionId);
    writeLe64(output + 12, payloadBytes);
    return packetBytes;
}

size_t buildDataPacket(uint32_t transactionId,
                       const uint8_t* payload,
                       size_t payloadBytes,
                       uint8_t* output,
                       size_t capacity) {
    constexpr size_t fixedBytes = 12;
    if (output == nullptr || payload == nullptr || payloadBytes == 0 ||
        payloadBytes > UINT32_MAX - fixedBytes) {
        return 0;
    }
    const size_t packetBytes = fixedBytes + payloadBytes;
    if (writeHeader(PacketType::Data,
                    static_cast<uint32_t>(packetBytes),
                    output,
                    capacity) == 0) {
        return 0;
    }
    writeLe32(output + 8, transactionId);
    std::memcpy(output + fixedBytes, payload, payloadBytes);
    return packetBytes;
}

size_t buildEndDataPacket(uint32_t transactionId,
                          uint8_t* output,
                          size_t capacity) {
    constexpr size_t packetBytes = 12;
    if (writeHeader(PacketType::EndData, packetBytes, output, capacity) == 0) {
        return 0;
    }
    writeLe32(output + 8, transactionId);
    return packetBytes;
}

size_t buildProbeResponse(uint8_t* output, size_t capacity) {
    return writeHeader(PacketType::ProbeResponse, 8, output, capacity);
}

}  // namespace ptpip
}  // namespace alpha_buddy
