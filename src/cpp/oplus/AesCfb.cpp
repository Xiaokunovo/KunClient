#include "oplus/AesCfb.h"

#include <algorithm>
#include <cstring>

namespace swt::edl {
namespace {

// FIPS-197 S 盒。
constexpr quint8 kSBox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

constexpr int kBlockSize = 16;
constexpr int kRoundKeys = 176;   // 11 组 × 16 字节

quint8 xtime(quint8 value)
{
    return static_cast<quint8>((value << 1) ^ ((value & 0x80) ? 0x1B : 0x00));
}

void expandKey(const quint8 key[kBlockSize], quint8 roundKeys[kRoundKeys])
{
    std::memcpy(roundKeys, key, kBlockSize);
    quint8 rcon = 0x01;
    for (int offset = kBlockSize; offset < kRoundKeys; offset += 4) {
        quint8 word[4] = { roundKeys[offset - 4], roundKeys[offset - 3],
                           roundKeys[offset - 2], roundKeys[offset - 1] };
        if (offset % kBlockSize == 0) {
            const quint8 first = word[0];
            word[0] = static_cast<quint8>(kSBox[word[1]] ^ rcon);
            word[1] = kSBox[word[2]];
            word[2] = kSBox[word[3]];
            word[3] = kSBox[first];
            rcon = xtime(rcon);
        }
        for (int i = 0; i < 4; ++i) {
            roundKeys[offset + i] = static_cast<quint8>(roundKeys[offset - kBlockSize + i] ^ word[i]);
        }
    }
}

void addRoundKey(quint8 state[kBlockSize], const quint8* roundKey)
{
    for (int i = 0; i < kBlockSize; ++i) state[i] = static_cast<quint8>(state[i] ^ roundKey[i]);
}

void subBytes(quint8 state[kBlockSize])
{
    for (int i = 0; i < kBlockSize; ++i) state[i] = kSBox[state[i]];
}

// state 按列优先排布：state[c * 4 + r]。
void shiftRows(quint8 state[kBlockSize])
{
    quint8 t = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = t;

    t = state[2];
    state[2] = state[10];
    state[10] = t;
    t = state[6];
    state[6] = state[14];
    state[14] = t;

    t = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = t;
}

void mixColumns(quint8 state[kBlockSize])
{
    for (int column = 0; column < 4; ++column) {
        quint8* c = state + column * 4;
        const quint8 a0 = c[0];
        const quint8 a1 = c[1];
        const quint8 a2 = c[2];
        const quint8 a3 = c[3];
        const quint8 all = static_cast<quint8>(a0 ^ a1 ^ a2 ^ a3);
        c[0] = static_cast<quint8>(c[0] ^ all ^ xtime(static_cast<quint8>(a0 ^ a1)));
        c[1] = static_cast<quint8>(c[1] ^ all ^ xtime(static_cast<quint8>(a1 ^ a2)));
        c[2] = static_cast<quint8>(c[2] ^ all ^ xtime(static_cast<quint8>(a2 ^ a3)));
        c[3] = static_cast<quint8>(c[3] ^ all ^ xtime(static_cast<quint8>(a3 ^ a0)));
    }
}

void encryptWithRoundKeys(const quint8 in[kBlockSize], quint8 out[kBlockSize],
                          const quint8 roundKeys[kRoundKeys])
{
    quint8 state[kBlockSize];
    std::memcpy(state, in, kBlockSize);
    addRoundKey(state, roundKeys);
    for (int round = 1; round < 10; ++round) {
        subBytes(state);
        shiftRows(state);
        mixColumns(state);
        addRoundKey(state, roundKeys + round * kBlockSize);
    }
    subBytes(state);
    shiftRows(state);
    addRoundKey(state, roundKeys + 10 * kBlockSize);
    std::memcpy(out, state, kBlockSize);
}

/// CFB128 主循环。encrypting=false 时做解密（反馈用密文），true 时做加密（反馈用密文=输出）。
QByteArray cfbTransform(const QByteArray& data, const QByteArray& key, const QByteArray& iv,
                        bool encrypting)
{
    if (key.size() != kBlockSize || iv.size() != kBlockSize || data.isEmpty()) return {};

    quint8 roundKeys[kRoundKeys];
    expandKey(reinterpret_cast<const quint8*>(key.constData()), roundKeys);

    QByteArray out(data.size(), '\0');
    quint8 feedback[kBlockSize];
    std::memcpy(feedback, iv.constData(), kBlockSize);

    for (qsizetype offset = 0; offset < data.size(); offset += kBlockSize) {
        quint8 keyStream[kBlockSize];
        encryptWithRoundKeys(feedback, keyStream, roundKeys);

        const qsizetype count = std::min<qsizetype>(kBlockSize, data.size() - offset);
        quint8 nextFeedback[kBlockSize] = { 0 };
        for (qsizetype i = 0; i < count; ++i) {
            const auto input = static_cast<quint8>(data[offset + i]);
            const auto output = static_cast<quint8>(input ^ keyStream[i]);
            out[offset + i] = static_cast<char>(output);
            // 反馈的永远是**密文**：解密时是输入，加密时是输出。
            nextFeedback[i] = encrypting ? output : input;
        }
        std::memcpy(feedback, nextFeedback, kBlockSize);
    }
    return out;
}

} // namespace

void AesCfb::encryptBlock(const quint8 in[BlockSize], quint8 out[BlockSize],
                          const quint8 key[BlockSize])
{
    quint8 roundKeys[kRoundKeys];
    expandKey(key, roundKeys);
    encryptWithRoundKeys(in, out, roundKeys);
}

QByteArray AesCfb::decrypt(const QByteArray& data, const QByteArray& key, const QByteArray& iv)
{
    return cfbTransform(data, key, iv, false);
}

QByteArray AesCfb::encrypt(const QByteArray& data, const QByteArray& key, const QByteArray& iv)
{
    return cfbTransform(data, key, iv, true);
}

} // namespace swt::edl
