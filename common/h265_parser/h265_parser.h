/**
 * @file h265_parser.h
 * @brief H265 码流解析模块接口
 * 
 * 提供 H265 码流文件的解析功能，支持通过起始码分割帧
 */

#ifndef H265_PARSER_H
#define H265_PARSER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief H265 解析器结构体（不透明类型）
 */
typedef struct H265Parser_s H265Parser_t;

/**
 * @brief H265 帧信息结构体
 */
typedef struct {
    const unsigned char *data;  // 帧数据指针（指向模块内部缓冲区，用户需要及时拷贝）
    size_t size;                // 帧大小（字节）
    int startCodeLen;           // 起始码长度（3 或 4）
} H265Frame_t;

/**
 * @brief 创建 H265 解析器
 * 
 * @param filePath H265 文件路径
 * @param readBufSize 读取缓冲区大小（字节），0 使用默认值（1MB）
 * @param loopEn 是否循环读取（1=循环，0=不循环，EOF 时返回）
 * @return 成功返回解析器指针，失败返回 NULL
 */
H265Parser_t* H265ParserCreate(const char *filePath, size_t readBufSize, int loopEn);

/**
 * @brief 销毁 H265 解析器
 * 
 * @param parser 解析器指针
 */
void H265ParserDestroy(H265Parser_t *parser);

/**
 * @brief 获取下一帧数据
 * 
 * @param parser 解析器指针
 * @param frame 输出参数，返回帧信息（data 指向模块内部缓冲区）
 * @return 成功返回 0，EOF 返回 1（仅在 loopEn=0 时），错误返回 -1
 * 
 * @note 返回的帧数据指针指向模块内部缓冲区，用户需要及时拷贝数据，
 *       因为下次调用 H265ParserNextFrame 时可能会覆盖缓冲区内容
 */
int H265ParserNextFrame(H265Parser_t *parser, H265Frame_t *frame);

/**
 * @brief H.265 NALU类型定义
 */
#define H265_NALU_TYPE_VPS 32
#define H265_NALU_TYPE_SPS 33
#define H265_NALU_TYPE_PPS 34
#define H265_NALU_TYPE_SEI 39
#define H265_NALU_TYPE_IDR_N_LP 19  // CRA (Clean Random Access)
#define H265_NALU_TYPE_IDR_W_RADL 20 // IDR with RADL
#define H265_NALU_TYPE_CRA_NUT 21    // CRA NUT
#define H265_NALU_TYPE_TRAIL_N 0     // TRAIL_N
#define H265_NALU_TYPE_TRAIL_R 1     // TRAIL_R
#define H265_NALU_TYPE_TSA_N 2       // TSA_N
#define H265_NALU_TYPE_TSA_R 3       // TSA_R
#define H265_NALU_TYPE_STSA_N 4      // STSA_N
#define H265_NALU_TYPE_STSA_R 5      // STSA_R
#define H265_NALU_TYPE_RADL_N 6      // RADL_N
#define H265_NALU_TYPE_RADL_R 7      // RADL_R
#define H265_NALU_TYPE_RASL_N 8      // RASL_N
#define H265_NALU_TYPE_RASL_R 9      // RASL_R

/**
 * @brief 获取H.265起始码长度
 * 
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 起始码长度（3或4），如果没有起始码返回0
 */
int H265GetStartCodeLen(const unsigned char *data, int len);

/**
 * @brief 获取NALU类型
 * 
 * @param data H.265数据（包含起始码）
 * @param len 数据长度
 * @return NALU类型，失败返回-1
 */
int H265GetNALUType(const unsigned char *data, int len);

/**
 * @brief 获取slice类型（I/P/B）
 * 
 * @param data NALU数据（包含起始码）
 * @param len 数据长度
 * @return 'I'表示I slice，'P'表示P slice，'B'表示B slice，失败返回-1
 */
int H265GetSliceType(const unsigned char *data, int len);

/**
 * @brief H.265帧分类器结构体（不透明类型）
 * 
 * 用于缓存VPS/SPS/PPS/SEI，并在遇到IDR帧时组合成完整的I帧
 */
typedef struct H265FrameClassifier_s H265FrameClassifier_t;

/**
 * @brief 帧分类结果
 */
typedef enum {
    H265_FRAME_TYPE_NONE = 0,    // 无帧（如VPS/SPS/PPS/SEI被缓存）
    H265_FRAME_TYPE_I,            // I帧（组合后的VPS+SPS+PPS+SEI+IDR）
    H265_FRAME_TYPE_P,            // P帧
    H265_FRAME_TYPE_B,            // B帧
    H265_FRAME_TYPE_OTHER         // 其他类型帧
} H265FrameType_t;

/**
 * @brief 分类后的帧信息
 */
typedef struct {
    const unsigned char *data;   // 帧数据指针（指向分类器内部缓冲区，需要及时拷贝）
    size_t size;                  // 帧大小
    H265FrameType_t type;         // 帧类型
} H265ClassifiedFrame_t;

/**
 * @brief 创建H.265帧分类器
 * 
 * @param combinedFrameBufSize 组合帧缓冲区大小（字节），0使用默认值（4MB）
 * @return 成功返回分类器指针，失败返回NULL
 */
H265FrameClassifier_t* H265FrameClassifierCreate(size_t combinedFrameBufSize);

/**
 * @brief 销毁H.265帧分类器
 * 
 * @param classifier 分类器指针
 */
void H265FrameClassifierDestroy(H265FrameClassifier_t *classifier);

/**
 * @brief 处理帧并分类
 * 
 * @param classifier 分类器指针
 * @param frame 输入的H.265帧（包含起始码）
 * @param output 输出参数，返回分类后的帧信息
 * @return 成功返回0，需要继续处理返回1（如VPS/SPS/PPS/SEI被缓存），错误返回-1
 * 
 * @note 返回的帧数据指针指向分类器内部缓冲区，用户需要及时拷贝数据
 */
int H265FrameClassifierProcess(H265FrameClassifier_t *classifier, 
                                const H265Frame_t *frame, 
                                H265ClassifiedFrame_t *output);

#ifdef __cplusplus
}
#endif

#endif /* H265_PARSER_H */

