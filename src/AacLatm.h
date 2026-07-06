#ifndef INCLUDE_AAC_LATM_H
#define INCLUDE_AAC_LATM_H

#include <vector>

// LATM(LOAS)のAudioSpecificConfig情報。
// フレーム間で使い回されるため(useSameStreamMux)、呼び出し側で保持しておくこと。
struct LatmAudioConfig
{
    bool  fValid;                  // 一度でもStreamMuxConfigを取得できたか
    int   audioObjectType;         // 2 = AAC LC
    int   samplingFrequencyIndex;  // ADTSのsampling_frequency_indexと同じ表
    int   channelConfiguration;
    int   frameLengthType;         // 0以外は未対応(フォールバックする)

    LatmAudioConfig() : fValid(false), audioObjectType(0),
        samplingFrequencyIndex(0), channelConfiguration(0), frameLengthType(-1) {}
};

// PES(1個ぶん)の中に入っているLOAS形式(0x56同期ヘッダ付き)のAudioMuxElementを、
// ADTS形式(7byteヘッダ+生AAC)に変換する。
// pData/sizeは、PESヘッダを除いたエレメンタリストリーム部分(LOAS同期バイトから始まる)。
// 変換に成功すればtrueを返し、outにADTS形式のバイト列を追加する。
// 失敗時(未知の形式・パース不能)はfalseを返す(呼び出し側は元データをそのまま使うこと)。
bool ConvertLatmToAdts(const unsigned char *pData, int size, LatmAudioConfig &config,
                        std::vector<unsigned char> &out);

#endif // INCLUDE_AAC_LATM_H
