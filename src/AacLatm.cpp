#include <Windows.h>
#include <cstring>
#include "AacLatm.h"

namespace {

// 簡易ビットリーダー(範囲外に達したらfOverrunを立てて0を返し続ける)
class CBitReader
{
public:
    CBitReader(const unsigned char *pData, int sizeBytes)
        : m_pData(pData), m_sizeBits(sizeBytes * 8), m_pos(0), m_fOverrun(false) {}

    unsigned int ReadBits(int n)
    {
        unsigned int v = 0;
        for (int i = 0; i < n; ++i) {
            v = (v << 1) | ReadBit();
        }
        return v;
    }
    unsigned int ReadBit()
    {
        if (m_pos >= m_sizeBits) {
            m_fOverrun = true;
            return 0;
        }
        int byteIdx = m_pos >> 3;
        int bitIdx = 7 - (m_pos & 7);
        unsigned int b = (m_pData[byteIdx] >> bitIdx) & 1;
        ++m_pos;
        return b;
    }
    bool IsOverrun() const { return m_fOverrun; }
    int BytePos() const { return (m_pos + 7) / 8; }   // 切り上げバイト位置
    int BitPos() const { return m_pos; }

private:
    const unsigned char *m_pData;
    int m_sizeBits;
    int m_pos;
    bool m_fOverrun;
};

// AudioSpecificConfig()の簡易解析(GASpecificConfigまで)
// 成功時true。SBR拡張等の複雑な構成は非対応(falseを返す)
bool ParseAudioSpecificConfig(CBitReader &br, LatmAudioConfig &cfg)
{
    int aot = br.ReadBits(5);
    if (aot == 31) {
        aot = 32 + br.ReadBits(6);
    }
    int sfi = br.ReadBits(4);
    if (sfi == 0x0f) {
        // 明示的なサンプリング周波数(24bit)は非対応
        br.ReadBits(24);
        return false;
    }
    int chan = br.ReadBits(4);

    // GASpecificConfig (AAC LC/LTP/SSR/Scalable共通の先頭部分)
    if (aot == 1 || aot == 2 || aot == 3 || aot == 4 || aot == 6 || aot == 7 || aot == 17 ||
        aot == 19 || aot == 20 || aot == 21 || aot == 22 || aot == 23)
    {
        br.ReadBits(1); // frameLengthFlag
        int dependsOnCoreCoder = br.ReadBits(1);
        if (dependsOnCoreCoder) {
            br.ReadBits(14); // coreCoderDelay
        }
        br.ReadBits(1); // extensionFlag (拡張の中身は非対応、以降は読まない)
    }
    else {
        // 上記以外のオブジェクトタイプは非対応
        return false;
    }

    if (br.IsOverrun()) return false;

    cfg.fValid = true;
    cfg.audioObjectType = aot;
    cfg.samplingFrequencyIndex = sfi;
    cfg.channelConfiguration = chan;
    return true;
}

// LatmGetValue(): 可変長の整数値取得(taraBufferFullness等で使用)
unsigned int LatmGetValue(CBitReader &br)
{
    int bitsForValue = br.ReadBits(2);
    unsigned int value = 0;
    for (int i = 0; i <= bitsForValue; ++i) {
        value = (value << 8) | br.ReadBits(8);
    }
    return value;
}

// StreamMuxConfig()の解析。プログラム/レイヤーは常に1個のみ対応(実放送で使われる構成)。
// 成功時、cfgを更新しtrueを返す。
bool ParseStreamMuxConfig(CBitReader &br, LatmAudioConfig &cfg)
{
    int audioMuxVersion = br.ReadBits(1);
    int audioMuxVersionA = 0;
    if (audioMuxVersion == 1) {
        audioMuxVersionA = br.ReadBits(1);
    }
    if (audioMuxVersionA != 0) return false; // 予約領域、非対応

    if (audioMuxVersion == 1) {
        LatmGetValue(br); // taraBufferFullness
    }

    br.ReadBits(1); // allStreamsSameTimeFraming
    br.ReadBits(6); // numSubFrames (複数サブフレームは非対応。0前提で以降処理)
    int numProgram = br.ReadBits(4);
    if (numProgram != 0) return false; // 複数プログラムは非対応

    int numLayer = br.ReadBits(3);
    if (numLayer != 0) return false; // 複数レイヤーは非対応

    // prog==0, lay==0 は useSameConfig を送らず必ずAudioSpecificConfigが来る
    if (!ParseAudioSpecificConfig(br, cfg)) return false;

    int frameLengthType = br.ReadBits(3);
    cfg.frameLengthType = frameLengthType;
    if (frameLengthType == 0) {
        br.ReadBits(8); // latmBufferFullness
    }
    else if (frameLengthType == 1) {
        br.ReadBits(9); // frameLength
    }
    else {
        return false; // その他のframeLengthTypeは非対応
    }

    int otherDataPresent = br.ReadBits(1);
    if (otherDataPresent) return false; // 非対応

    int crcCheckPresent = br.ReadBits(1);
    if (crcCheckPresent) {
        br.ReadBits(8);
    }

    return !br.IsOverrun();
}

// PayloadLengthInfo() + 生AACデータ位置の取得(単一プログラム/レイヤー前提)
bool ParsePayloadLength(CBitReader &br, const LatmAudioConfig &cfg, int &payloadLen)
{
    if (cfg.frameLengthType == 0) {
        int total = 0;
        for (;;) {
            int b = br.ReadBits(8);
            if (br.IsOverrun()) return false;
            total += b;
            if (b != 255) break;
        }
        payloadLen = total;
        return true;
    }
    // frameLengthType==1(固定長)は今回のフォーマットでは未確認のため非対応
    return false;
}

// srcのstartBitビット目からnumBits分(8の倍数であること)を、バイト境界を無視して抽出し、
// 左詰め(MSBから)でoutへ書き込む。out は resize 済みであること。
// (旧実装は1bitずつループしていたため、8bit単位でまとめて処理するよう最適化)
void CopyBits(const unsigned char *src, int startBit, int numBits, unsigned char *out)
{
    int numBytes = numBits / 8; // 呼び出し側は常に8の倍数のnumBitsを渡す
    int byteOffset = startBit >> 3;
    int bitShift = startBit & 7;

    if (bitShift == 0) {
        memcpy(out, src + byteOffset, numBytes);
        return;
    }

    const unsigned char *p = src + byteOffset;
    for (int i = 0; i < numBytes; ++i) {
        out[i] = (unsigned char)((p[i] << bitShift) | (p[i + 1] >> (8 - bitShift)));
    }
}

} // namespace

bool ConvertLatmToAdts(const unsigned char *pData, int size, LatmAudioConfig &config,
                        std::vector<unsigned char> &out)
{
    // LOAS同期ヘッダ(3byte): 0x56, 0xE0|len[12:8], len[7:0]
    if (size < 4 || pData[0] != 0x56 || (pData[1] & 0xE0) != 0xE0) {
        return false;
    }
    int audioMuxLengthBytes = ((pData[1] & 0x1f) << 8) | pData[2];
    if (audioMuxLengthBytes <= 0 || 3 + audioMuxLengthBytes > size) {
        return false;
    }

    const unsigned char *pAmux = pData + 3;
    CBitReader br(pAmux, audioMuxLengthBytes);

    unsigned int useSameStreamMux = br.ReadBit();
    if (!useSameStreamMux) {
        LatmAudioConfig newCfg;
        if (!ParseStreamMuxConfig(br, newCfg)) return false;
        config = newCfg;
    }
    else {
        if (!config.fValid) return false; // 設定が来る前にreuseされても復元不能
    }

    int payloadLen = 0;
    if (!ParsePayloadLength(br, config, payloadLen)) return false;

    // 注意: LATM/LOASは純粋なビットストリームであり、ここまでの消費ビット数が
    // バイト境界(8の倍数)に一致するとは限らない。バイト単位に丸めてはいけない。
    int payloadStartBit = br.BitPos();
    if (payloadStartBit + payloadLen * 8 > audioMuxLengthBytes * 8) return false;

    // ADTSヘッダー(7byte、CRC無し)を組み立てる
    // profile = audioObjectType - 1 (AAC LC(2) -> profile 1)
    int profile = config.audioObjectType - 1;
    if (profile < 0 || profile > 3) return false;

    int frameLength = 7 + payloadLen;
    unsigned char adts[7];
    adts[0] = 0xFF;
    adts[1] = 0xF1; // MPEG-4, Layer=00, protection_absent=1
    adts[2] = (unsigned char)(((profile & 0x3) << 6) |
                               ((config.samplingFrequencyIndex & 0xF) << 2) |
                               ((config.channelConfiguration >> 2) & 0x1));
    adts[3] = (unsigned char)(((config.channelConfiguration & 0x3) << 6) |
                               ((frameLength >> 11) & 0x3));
    adts[4] = (unsigned char)((frameLength >> 3) & 0xFF);
    adts[5] = (unsigned char)(((frameLength & 0x7) << 5) | 0x1F);
    adts[6] = 0xFC;

    size_t base = out.size();
    out.resize(base + 7 + payloadLen, 0);
    memcpy(&out[base], adts, 7);
    CopyBits(pAmux, payloadStartBit, payloadLen * 8, &out[base + 7]);
    return true;
}
