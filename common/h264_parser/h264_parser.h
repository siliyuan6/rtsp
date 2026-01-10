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

/**
 * @brief H.264 NALU类型定义
 */
#define H264_NALU_TYPE_SEI 6
#define H264_NALU_TYPE_SPS 7
#define H264_NALU_TYPE_PPS 8
#define H264_NALU_TYPE_IDR 5
#define H264_NALU_TYPE_NON_IDR 1

/**
 * @brief 获取H.264起始码长度
 * 
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 起始码长度（3或4），如果没有起始码返回0
 */
int H264GetStartCodeLen(const unsigned char *data, int len);

/**
 * @brief 获取NALU类型
 * 
 * @param data H.264数据（包含起始码）
 * @param len 数据长度
 * @return NALU类型，失败返回-1
 */
int H264GetNALUType(const unsigned char *data, int len);

/**
 * @brief 获取slice类型（I/P/B）
 * 
 * @param data NALU数据（包含起始码）
 * @param len 数据长度
 * @return 'I'表示I slice，'P'表示P slice，'B'表示B slice，失败返回-1
 */
int H264GetSliceType(const unsigned char *data, int len);

/**
 * @brief H.264帧分类器结构体（不透明类型）
 * 
 * 用于缓存SPS/PPS/SEI，并在遇到IDR帧时组合成完整的I帧
 */
typedef struct H264FrameClassifier_s H264FrameClassifier_t;

/**
 * @brief 帧分类结果
 */
typedef enum {
    H264_FRAME_TYPE_NONE = 0,    // 无帧（如SPS/PPS/SEI被缓存）
    H264_FRAME_TYPE_I,            // I帧（组合后的SPS+PPS+SEI+IDR）
    H264_FRAME_TYPE_P,            // P帧
    H264_FRAME_TYPE_B,            // B帧
    H264_FRAME_TYPE_OTHER         // 其他类型帧
} H264FrameType_t;

/**
 * @brief 分类后的帧信息
 */
typedef struct {
    const unsigned char *data;   // 帧数据指针（指向分类器内部缓冲区，需要及时拷贝）
    size_t size;                  // 帧大小
    H264FrameType_t type;         // 帧类型
} H264ClassifiedFrame_t;

/**
 * @brief 创建H.264帧分类器
 * 
 * @param combinedFrameBufSize 组合帧缓冲区大小（字节），0使用默认值（4MB）
 * @return 成功返回分类器指针，失败返回NULL
 */
H264FrameClassifier_t* H264FrameClassifierCreate(size_t combinedFrameBufSize);

/**
 * @brief 销毁H.264帧分类器
 * 
 * @param classifier 分类器指针
 */
void H264FrameClassifierDestroy(H264FrameClassifier_t *classifier);

/**
 * @brief 处理帧并分类
 * 
 * @param classifier 分类器指针
 * @param frame 输入的H.264帧（包含起始码）
 * @param output 输出参数，返回分类后的帧信息
 * @return 成功返回0，需要继续处理返回1（如SPS/PPS/SEI被缓存），错误返回-1
 * 
 * @note 返回的帧数据指针指向分类器内部缓冲区，用户需要及时拷贝数据
 */
int H264FrameClassifierProcess(H264FrameClassifier_t *classifier, 
                                const H264Frame_t *frame, 
                                H264ClassifiedFrame_t *output);

#ifdef __cplusplus
}
#endif

#endif /* H264_PARSER_H */
