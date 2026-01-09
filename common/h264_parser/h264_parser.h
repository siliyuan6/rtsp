/**
 * @file h264_parser.h
 * @brief H264 码流解析模块接口
 * 
 * 提供 H264 码流文件的解析功能，支持通过起始码分割帧
 */

#ifndef H264_PARSER_H
#define H264_PARSER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief H264 解析器结构体（不透明类型）
 */
typedef struct H264Parser_s H264Parser_t;

/**
 * @brief H264 帧信息结构体
 */
typedef struct {
    const unsigned char *data;  // 帧数据指针（指向模块内部缓冲区，用户需要及时拷贝）
    size_t size;                // 帧大小（字节）
    int startCodeLen;           // 起始码长度（3 或 4）
} H264Frame_t;

/**
 * @brief 创建 H264 解析器
 * 
 * @param filePath H264 文件路径
 * @param readBufSize 读取缓冲区大小（字节），0 使用默认值（1MB）
 * @param loopEn 是否循环读取（1=循环，0=不循环，EOF 时返回）
 * @return 成功返回解析器指针，失败返回 NULL
 */
H264Parser_t* H264ParserCreate(const char *filePath, size_t readBufSize, int loopEn);

/**
 * @brief 销毁 H264 解析器
 * 
 * @param parser 解析器指针
 */
void H264ParserDestroy(H264Parser_t *parser);

/**
 * @brief 获取下一帧数据
 * 
 * @param parser 解析器指针
 * @param frame 输出参数，返回帧信息（data 指向模块内部缓冲区）
 * @return 成功返回 0，EOF 返回 1（仅在 loopEn=0 时），错误返回 -1
 * 
 * @note 返回的帧数据指针指向模块内部缓冲区，用户需要及时拷贝数据，
 *       因为下次调用 H264ParserNextFrame 时可能会覆盖缓冲区内容
 */
int H264ParserNextFrame(H264Parser_t *parser, H264Frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* H264_PARSER_H */
