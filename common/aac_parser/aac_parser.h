/**
 * @file aac_parser.h
 * @brief AAC 码流解析模块接口
 * 
 * 提供 AAC 码流文件的解析功能，支持通过 ADTS 帧头分割帧
 */

#ifndef AAC_PARSER_H
#define AAC_PARSER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AAC 解析器结构体（不透明类型）
 */
typedef struct AACParser_s AACParser_t;

/**
 * @brief ADTS 帧信息结构体
 */
typedef struct {
    int frameLength;           // 帧长度（字节）
    int protectionAbsent;      // 保护位（0=有CRC，1=无CRC）
    int profile;               // Profile (0=Main, 1=LC, 2=SSR, 3=LTP)
    int samplingFrequencyIndex; // 采样率索引
    int channelConfig;         // 声道配置
    int original;              // 原始标志
    int home;                  // Home标志
} ADTSHeaderInfo_t;

/**
 * @brief AAC 帧信息结构体
 */
typedef struct {
    const unsigned char *data;  // 帧数据指针（指向模块内部缓冲区，用户需要及时拷贝）
    size_t size;                // 帧大小（字节）
    ADTSHeaderInfo_t headerInfo; // ADTS头部信息
} AACFrame_t;

/**
 * @brief 创建 AAC 解析器
 * 
 * @param filePath AAC 文件路径
 * @param readBufSize 读取缓冲区大小（字节），0 使用默认值（1MB）
 * @param loopEn 是否循环读取（1=循环，0=不循环，EOF 时返回）
 * @return 成功返回解析器指针，失败返回 NULL
 */
AACParser_t* AACParserCreate(const char *filePath, size_t readBufSize, int loopEn);

/**
 * @brief 销毁 AAC 解析器
 * 
 * @param parser 解析器指针
 */
void AACParserDestroy(AACParser_t *parser);

/**
 * @brief 获取下一帧数据
 * 
 * @param parser 解析器指针
 * @param frame 输出参数，返回帧信息（data 指向模块内部缓冲区）
 * @return 成功返回 0，EOF 返回 1（仅在 loopEn=0 时），错误返回 -1
 * 
 * @note 返回的帧数据指针指向模块内部缓冲区，用户需要及时拷贝数据，
 *       因为下次调用 AACParserNextFrame 时可能会覆盖缓冲区内容
 */
int AACParserNextFrame(AACParser_t *parser, AACFrame_t *frame);

/**
 * @brief 解析 ADTS 头部
 * 
 * @param data ADTS 帧数据（包含同步字）
 * @param len 数据长度
 * @param headerInfo 输出参数，返回解析后的头部信息
 * @return 成功返回头部长度（7或9字节），失败返回-1
 */
int AACParseADTSHeader(const unsigned char *data, int len, ADTSHeaderInfo_t *headerInfo);

#ifdef __cplusplus
}
#endif

#endif /* AAC_PARSER_H */

