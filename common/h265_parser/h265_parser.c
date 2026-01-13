/**
 * @file h265_parser.c
 * @brief H265 码流解析模块实现
 */

#include "h265_parser.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief H265 解析器内部结构体
 */
struct H265Parser_s {
    FILE *fp;                  // 文件指针
    unsigned char *buffer;     // 读取缓冲区
    size_t bufferSize;         // 缓冲区大小
    size_t readPos;            // 读取位置
    size_t validDataSize;      // 缓冲区中有效数据大小
    int loopEn;                // 是否循环读取
};

#define DEFAULT_BUFFER_SIZE (1024 * 1024)  // 默认1MB

/**
 * @brief 验证是否是有效的 NALU 起始码
 * 
 * 检查起始码后的 NALU 头是否有效，避免帧数据中的 0x00000001 被误判为起始码
 * H.265 使用 2 字节的 NALU 头
 * 
 * @param data 数据缓冲区
 * @param size 缓冲区大小
 * @param startcode_pos 起始码位置
 * @param startCodeLen 起始码长度（3或4）
 * @return 有效返回1，无效返回0
 */
static int IsValidStartCode(const unsigned char *data, size_t size, int startcode_pos, int startCodeLen)
{
    if (startcode_pos < 0 || startCodeLen < 3 || startCodeLen > 4)
    {
        return 0;
    }
    
    int nalu_header_pos = startcode_pos + startCodeLen;
    
    // 检查是否有足够的空间存放 NALU 头（H.265 需要 2 字节）
    if (nalu_header_pos + 1 >= (int)size)
    {
        return 0;
    }
    
    // H.265 NALU 头格式：2 字节
    // 第一个字节：[forbidden_zero_bit(1) | nal_unit_type(6) | nuh_layer_id(6) | nuh_temporal_id_plus1(3)]
    // 提取 NALU 类型（第一个字节的低 6 位）
    unsigned char nalu_header_byte0 = data[nalu_header_pos];
    int nalu_type = (nalu_header_byte0 >> 1) & 0x3F;
    
    // 验证 NALU 类型是否在有效范围内（0-63）
    if (nalu_type < 0 || nalu_type > 63)
    {
        return 0; // NALU 类型无效
    }
    
    // 增强验证：检查起始码前的数据
    if (startcode_pos > 0)
    {
        // 对于3字节起始码，检查前一个字节不应该也是0x00
        if (startCodeLen == 3 && data[startcode_pos - 1] == 0x00)
        {
            // 可能是 0x00 0x00 0x00 0x01 的前3字节，应该使用4字节起始码
            return 0;
        }
        
        // 检查起始码前是否有连续的0x00
        int check_pos = startcode_pos - 1;
        int zero_count = 0;
        while (check_pos >= 0 && check_pos >= startcode_pos - 4 && data[check_pos] == 0x00)
        {
            zero_count++;
            check_pos--;
        }
        
        // 如果起始码前有3个或更多连续的0x00，很可能是帧数据的一部分
        if (zero_count >= 3)
        {
            return 0;
        }
    }
    
    return 1; // 通过所有验证，是有效的起始码
}

/**
 * @brief 查找 H265 起始码
 * 
 * @param data 数据缓冲区
 * @param size 缓冲区大小
 * @param offset 起始偏移量
 * @param startCodeLen 输出参数，返回起始码长度（3或4），可为NULL
 * @return 找到有效起始码返回起始码的开始位置，未找到返回-1
 */
static int FindStartCode(const unsigned char *data, size_t size, int offset, int *startCodeLen)
{
    if (offset < 0 || (size_t)offset >= size)
    {
        return -1;
    }

    int i;
    for (i = offset; i < (int)size - 3; i++)
    {
        // 查找 0x00 0x00 0x00 0x01 或 0x00 0x00 0x01
        if (data[i] == 0x00 && data[i+1] == 0x00)
        {
            // 优先检查4字节起始码
            if (i + 3 < (int)size && data[i+2] == 0x00 && data[i+3] == 0x01)
            {
                // 验证是否是有效的起始码
                if (IsValidStartCode(data, size, i, 4))
                {
                    if (startCodeLen != NULL)
                    {
                        *startCodeLen = 4;
                    }
                    return i; // 返回起始码的开始位置
                }
            }
            // 检查3字节起始码
            else if (i + 2 < (int)size && data[i+2] == 0x01)
            {
                // 对于3字节起始码，确保前面不是0x00（避免误判4字节起始码的前3字节）
                if (i == 0 || data[i - 1] != 0x00)
                {
                    // 验证是否是有效的起始码
                    if (IsValidStartCode(data, size, i, 3))
                    {
                        if (startCodeLen != NULL)
                        {
                            *startCodeLen = 3;
                        }
                        return i; // 返回起始码的开始位置
                    }
                }
            }
        }
    }
    return -1;
}

/**
 * @brief 从文件读取数据到缓冲区
 * 
 * @param parser 解析器指针
 * @return 成功返回读取的字节数，EOF返回0，错误返回-1
 */
static int ReadBufferFromFile(H265Parser_t *parser)
{
    // 读取数据到缓冲区
    size_t readSize = fread(parser->buffer + parser->validDataSize, 
                            1, 
                            parser->bufferSize - parser->validDataSize, 
                            parser->fp);
    if (readSize == 0)
    {
        // 文件读取完毕
        if (feof(parser->fp))
        {
            if (parser->loopEn)
            {
                // 循环读取：重新开始
                LOG_DEBUG("[H265Parser] File EOF reached, restarting from beginning...\n");
                fseek(parser->fp, 0, SEEK_SET);
                clearerr(parser->fp);
                // 重新读取
                readSize = fread(parser->buffer + parser->validDataSize, 
                            1, 
                            parser->bufferSize - parser->validDataSize, 
                            parser->fp);
                if (readSize == 0)
                {
                    LOG_WARN("[H265Parser] Empty file after loop restart\n");
                    return 0;
                }
                parser->validDataSize += readSize;
                LOG_DEBUG("[H265Parser] Read %zu bytes after loop restart\n", readSize);
                return (int)readSize;
            }
            else
            {
                // 不循环：返回EOF
                LOG_DEBUG("[H265Parser] File EOF reached (no loop)\n");
                return 0;
            }
        }
        else
        {
            // 读取错误
            LOG_ERR("[H265Parser] File read error: %d\n", ferror(parser->fp));
            return -1;
        }
    }
    
    parser->validDataSize += readSize;
    LOG_DEBUG("[H265Parser] Read %zu bytes from file\n", readSize);
    
    return (int)readSize;
}

H265Parser_t* H265ParserCreate(const char *filePath, size_t readBufSize, int loopEn)
{
    if (filePath == NULL)
    {
        LOG_ERR("[H265Parser] File path is NULL\n");
        return NULL;
    }

    // 分配解析器结构体
    H265Parser_t *parser = (H265Parser_t *)malloc(sizeof(H265Parser_t));
    if (parser == NULL)
    {
        LOG_ERR("[H265Parser] Failed to allocate parser structure\n");
        return NULL;
    }
    memset(parser, 0, sizeof(H265Parser_t));

    // 打开文件
    parser->fp = fopen(filePath, "rb");
    if (parser->fp == NULL)
    {
        LOG_ERR("[H265Parser] Failed to open file: %s\n", filePath);
        free(parser);
        return NULL;
    }

    // 设置缓冲区大小（如果为0则使用默认值1MB）
    if (readBufSize == 0)
    {
        readBufSize = DEFAULT_BUFFER_SIZE;
    }
    parser->bufferSize = readBufSize;

    // 分配缓冲区
    parser->buffer = (unsigned char *)malloc(parser->bufferSize);
    if (parser->buffer == NULL)
    {
        LOG_ERR("[H265Parser] Failed to allocate buffer\n");
        fclose(parser->fp);
        free(parser);
        return NULL;
    }
    memset(parser->buffer, 0, parser->bufferSize);

    // 初始化状态
    parser->validDataSize = 0;
    parser->loopEn = loopEn ? 1 : 0;

    LOG_INFO("[H265Parser] Created parser for file: %s, buffer size: %zu, loop: %d\n",
        filePath, parser->bufferSize, parser->loopEn);

    return parser;
}

void H265ParserDestroy(H265Parser_t *parser)
{
    if (parser == NULL)
    {
        return;
    }

    if (parser->fp != NULL)
    {
        fclose(parser->fp);
        parser->fp = NULL;
    }

    if (parser->buffer != NULL)
    {
        free(parser->buffer);
        parser->buffer = NULL;
    }

    free(parser);
    LOG_DEBUG("[H265Parser] Parser destroyed\n");
}

int H265ParserNextFrame(H265Parser_t *parser, H265Frame_t *frame)
{
    if (parser == NULL || frame == NULL)
    {
        LOG_ERR("[H265Parser] Invalid parameters\n");
        return -1;
    }

    // 如果缓冲区为空，先读取数据
    if (parser->validDataSize == 0)
    {
        int read_result = ReadBufferFromFile(parser);
        if (read_result <= 0)
        {
            if (read_result == 0)
            {
                // EOF
                return 1;
            }
            else
            {
                // 读取错误
                return -1;
            }
        }
    }

    // 步骤1: 找到第一个起始码
    int firstFrameStartCodeLen = 0;
    int firstFramePos = FindStartCode(parser->buffer, 
                                     parser->validDataSize, 
                                     parser->readPos, 
                                     &firstFrameStartCodeLen);
    if (firstFramePos < 0)
    {
        LOG_ERR("[H265Parser] First frame: no start code found, validDataSize=%zu, readPos=%zu\n", 
            parser->validDataSize, parser->readPos);
        return -1;
    }

    // 步骤2: 在第一个起始码后面开始再找到第二个起始码
    int searchStart = firstFramePos + firstFrameStartCodeLen;
    int secondFrameStartCodeLen = 0;
    int secondFramePos = FindStartCode(parser->buffer, 
                                       parser->validDataSize, 
                                       searchStart, 
                                       &secondFrameStartCodeLen);

    // 注意: 当第一个起始码找到了，第二个找不到，那就将第一个起始码往后的所有数据都挪到缓存区的最开头位置。
    // 然后再去继续读文件，将该缓冲区读满。然后再开始找第一个起始码和第二个起始码
    if (secondFramePos < 0)
    {
        // 第二个起始码没找到，移动数据到开头
        int remainingSize = parser->validDataSize - firstFramePos;
        if (remainingSize <= 0)
        {
            LOG_ERR("[H265Parser] No remaining data after first start code\n");
            return -1;
        }

        // 将第一个起始码及其后面的数据移动到缓冲区开头
        memmove(parser->buffer, 
                parser->buffer + firstFramePos, 
                remainingSize);
        parser->readPos = 0;
        parser->validDataSize = remainingSize;
        LOG_DEBUG("[H265Parser] Moved %d bytes to buffer start, continuing to read...\n", remainingSize);
        
        // 尝试读取更多数据
        int read_result = ReadBufferFromFile(parser);
        if (read_result < 0)
        {
            // 读取错误
            LOG_ERR("[H265Parser] Error reading file after moving data\n");
            return -1;
        }
        
        // 检查是否是文件结束且不再循环
        int is_eof = (read_result == 0 && feof(parser->fp) && !parser->loopEn);
        
        // 重新查找第一个和第二个起始码
        firstFramePos = 0;
        firstFrameStartCodeLen = 0;
        firstFramePos = FindStartCode(parser->buffer, parser->validDataSize, 0, &firstFrameStartCodeLen);
        if (firstFramePos < 0)
        {
            // 如果找不到第一个起始码，检查是否是文件结束
            if (is_eof)
            {
                // 文件已结束，将移动后的所有数据作为最后一帧
                if (parser->validDataSize > 0)
                {
                    frame->data = parser->buffer;
                    frame->size = parser->validDataSize;
                    frame->startCodeLen = 0; // 无法确定起始码长度
                    parser->validDataSize = 0;
                    parser->readPos = 0;
                    LOG_DEBUG("[H265Parser] Last frame from EOF (no start code found): size=%zu\n", frame->size);
                    return 0;
                }
            }
            // 如果找不到第一个起始码，说明剩余数据不完整，返回EOF
            LOG_DEBUG("[H265Parser] No first start code found after moving data, EOF\n");
            return 1;
        }
        
        searchStart = firstFramePos + firstFrameStartCodeLen;
        secondFramePos = FindStartCode(parser->buffer, 
                                       parser->validDataSize, 
                                       searchStart, 
                                       &secondFrameStartCodeLen);
        if (secondFramePos < 0)
        {
            // 仍然找不到第二个起始码
            if (is_eof)
            {
                // 文件已结束，将第一个起始码之后的所有数据作为最后一帧
                int lastFrameSize = parser->validDataSize - firstFramePos;
                if (lastFrameSize > 0)
                {
                    frame->data = parser->buffer + firstFramePos;
                    frame->size = lastFrameSize;
                    frame->startCodeLen = firstFrameStartCodeLen;
                    parser->validDataSize = 0;
                    parser->readPos = 0;
                    LOG_DEBUG("[H265Parser] Last frame from EOF: size=%d\n", lastFrameSize);
                    return 0;
                }
            }
            LOG_ERR("[H265Parser] No second start code found after reading more data, read_result=%d, eof=%d, loopEn=%d\n",
                read_result, feof(parser->fp) ? 1 : 0, parser->loopEn);
            return -1;
        }
    }

    // 步骤3: 两个起始码中间区域就是该帧
    int frameSize = secondFramePos - firstFramePos;
    if (frameSize <= 0)
    {
        LOG_ERR("[H265Parser] Invalid frame size: %d\n", frameSize);
        return -1;
    }
    
    // 先设置帧数据指针和大小（在 memmove 之前设置，确保指向正确的数据）
    frame->data = parser->buffer + firstFramePos;
    frame->size = frameSize;
    frame->startCodeLen = firstFrameStartCodeLen;
    
    // 更新 readPos 为第二个起始码位置，下次从这里开始查找
    parser->readPos = secondFramePos;
    
    // 检查是否需要移动数据到开头（当 readPos 超过缓冲区一半时）
    if (parser->readPos > parser->bufferSize / 2)
    {
        // 将第二个起始码之后的数据移到缓冲区开头
        size_t remainingSize = parser->validDataSize - parser->readPos;
        if (remainingSize > 0 && remainingSize <= parser->bufferSize)
        {
            // 检查帧数据是否会被 memmove 的目标区域覆盖
            if (firstFramePos < (int)remainingSize)
            {
                // 帧数据会被覆盖，不执行 memmove
                LOG_DEBUG("[H265Parser] Skipping memmove to avoid overwriting frame data: firstFramePos=%d, remainingSize=%zu\n",
                    firstFramePos, remainingSize);
            }
            else
            {
                // 帧数据不会被覆盖，可以安全执行 memmove
                memmove(parser->buffer, parser->buffer + parser->readPos, remainingSize);
                parser->validDataSize = remainingSize;
                parser->readPos = 0;
                
                LOG_DEBUG("[H265Parser] Moved %zu bytes to buffer start\n", remainingSize);
            }
        }
        else
        {
            parser->validDataSize = 0;
            parser->readPos = 0;
        }
    }
    // 如果 readPos 已经超出有效数据范围，需要读取更多数据
    else if (parser->readPos >= parser->validDataSize)
    {
        // readPos 已经超出，重置并从开头查找
        parser->readPos = 0;
        parser->validDataSize = 0;
    }

    return 0;
}

/**
 * @brief 获取H.265起始码长度
 * 
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 起始码长度（3或4），如果没有起始码返回0
 */
int H265GetStartCodeLen(const unsigned char *data, int len)
{
    if (len >= 4 && data[0] == 0 && data[1] == 0 &&
        data[2] == 0 && data[3] == 1)
    {
        return 4; // 4字节起始码 0x00000001
    }
    else if (len >= 3 && data[0] == 0 && data[1] == 0 &&
        data[2] == 1)
    {
        return 3; // 3字节起始码 0x000001
    }
    return 0; // 没有起始码
}

/**
 * @brief 获取NALU类型
 * 
 * @param data H.265数据（包含起始码）
 * @param len 数据长度
 * @return NALU类型，失败返回-1
 */
int H265GetNALUType(const unsigned char *data, int len)
{
    int startCodeLen = H265GetStartCodeLen(data, len);
    if (startCodeLen <= 0 || startCodeLen + 1 >= len)
    {
        return -1;
    }
    
    // H.265 NALU 头格式：2 字节
    // 第一个字节：[forbidden_zero_bit(1) | nal_unit_type(6) | nuh_layer_id(6) | nuh_temporal_id_plus1(3)]
    // NALU类型在第一个字节的低 6 位
    unsigned char nalu_header_byte0 = data[startCodeLen];
    return (nalu_header_byte0 >> 1) & 0x3F;
}

/**
 * @brief 解析exp-Golomb编码（按位读取）
 * 
 * @param data 数据缓冲区
 * @param len 数据长度（字节数）
 * @param bitOffset 当前位偏移量（输入输出参数，从0开始）
 * @return 解码后的值，失败返回-1
 */
static int ReadExpGolomb(const unsigned char *data, int len, int *bitOffset)
{
    if (NULL == data || len <= 0 || NULL == bitOffset)
    {
        return -1;
    }
    
    int leadingZeros = 0;
    int bitPos = *bitOffset;
    
    // 计算前导零的个数
    while (bitPos < len * 8)
    {
        int bytePos = bitPos / 8;
        int bitInByte = bitPos % 8;
        
        if (bytePos >= len)
        {
            return -1; // 数据不足
        }
        
        unsigned char byte = data[bytePos];
        int bit = (byte >> (7 - bitInByte)) & 0x01;
        
        if (bit == 1)
        {
            break; // 找到第一个1
        }
        
        leadingZeros++;
        bitPos++;
    }
    
    if (bitPos >= len * 8)
    {
        return -1; // 数据不足
    }
    
    // 读取leadingZeros+1位，计算值
    int value = 1 << leadingZeros;
    bitPos++; // 跳过第一个1
    
    for (int i = 0; i < leadingZeros; i++)
    {
        if (bitPos >= len * 8)
        {
            return -1; // 数据不足
        }
        
        int bytePos = bitPos / 8;
        int bitInByte = bitPos % 8;
        unsigned char byte = data[bytePos];
        int bit = (byte >> (7 - bitInByte)) & 0x01;
        
        value |= (bit << (leadingZeros - 1 - i));
        bitPos++;
    }
    
    *bitOffset = bitPos;
    return value - 1; // exp-Golomb编码的值需要减1
}

/**
 * @brief 获取slice类型（I/P/B）
 * 
 * H.265 需要解析 slice_segment_header 中的 slice_type
 * 
 * @param data NALU数据（包含起始码）
 * @param len 数据长度
 * @return 'I'表示I slice，'P'表示P slice，'B'表示B slice，失败返回-1
 */
int H265GetSliceType(const unsigned char *data, int len)
{
    int startCodeLen = H265GetStartCodeLen(data, len);
    if (startCodeLen <= 0 || startCodeLen + 2 >= len)
    {
        return -1;
    }
    
    // H.265 NALU 头为 2 字节
    int naluHeaderPos = startCodeLen;
    unsigned char nalu_header_byte0 = data[naluHeaderPos];
    int nalu_type = (nalu_header_byte0 >> 1) & 0x3F;
    
    // IDR 帧类型（19, 20, 21）直接返回 I
    if (nalu_type == H265_NALU_TYPE_IDR_N_LP || 
        nalu_type == H265_NALU_TYPE_IDR_W_RADL ||
        nalu_type == H265_NALU_TYPE_CRA_NUT)
    {
        return 'I';
    }
    
    // 只有 TRAIL/TSA/STSA/RADL/RASL 类型（0-9）需要解析 slice_segment_header
    if (nalu_type < 0 || nalu_type > 9)
    {
        return -1;
    }
    
    // slice_segment_header 从 NALU payload 开始（跳过 NALU 头 2 字节）
    int sliceHeaderStart = naluHeaderPos + 2;
    if (sliceHeaderStart >= len)
    {
        return -1;
    }
    
    // 解析 slice_segment_header
    // 需要跳过：first_slice_segment_in_pic_flag (1 bit)
    // 然后读取：slice_type (ue(v))
    // 注意：exp-Golomb 是按位编码的，需要按位读取
    int bitOffset = sliceHeaderStart * 8; // 转换为位偏移量
    
    // 跳过 first_slice_segment_in_pic_flag (1 bit)
    bitOffset++;
    
    // 读取 slice_type (exp-Golomb 编码)
    int sliceType = ReadExpGolomb(data, len, &bitOffset);
    if (sliceType < 0)
    {
        // 解析失败，默认当作 P 帧处理
        LOG_DEBUG("[H265GetSliceType] Failed to parse slice_type, defaulting to P frame\n");
        return 'P';
    }
    
    // slice_type 值映射（H.265）：
    // 0, 3: I slice
    // 1, 4: P slice
    // 2, 5: B slice
    if (sliceType == 0 || sliceType == 3)
    {
        return 'I';
    }
    else if (sliceType == 1 || sliceType == 4)
    {
        return 'P';
    }
    else if (sliceType == 2 || sliceType == 5)
    {
        return 'B';
    }
    
    // 未知的 slice_type，默认当作 P 帧
    LOG_DEBUG("[H265GetSliceType] Unknown slice_type: %d, defaulting to P frame\n", sliceType);
    return 'P';
}

/**
 * @brief H.265帧分类器内部结构体
 */
struct H265FrameClassifier_s {
    unsigned char *vpsBuf;          // VPS缓存缓冲区
    unsigned char *spsBuf;          // SPS缓存缓冲区
    unsigned char *ppsBuf;          // PPS缓存缓冲区
    unsigned char *seiBuf;          // SEI缓存缓冲区
    unsigned char *combinedFrameBuf; // 组合帧缓冲区
    int vpsLen;                     // VPS长度
    int spsLen;                     // SPS长度
    int ppsLen;                     // PPS长度
    int seiLen;                     // SEI长度
    int hasVps;                     // 是否有VPS
    int hasSps;                     // 是否有SPS
    int hasPps;                     // 是否有PPS
    int hasSei;                     // 是否有SEI
    size_t cacheBufSize;            // 缓存缓冲区大小
    size_t combinedBufSize;         // 组合缓冲区大小
};

#define DEFAULT_CACHE_BUF_SIZE (2 * 1024 * 1024)  // 默认2MB
#define DEFAULT_COMBINED_BUF_SIZE (4 * 1024 * 1024) // 默认4MB

H265FrameClassifier_t* H265FrameClassifierCreate(size_t combinedFrameBufSize)
{
    H265FrameClassifier_t *classifier = (H265FrameClassifier_t *)malloc(sizeof(H265FrameClassifier_t));
    if (classifier == NULL)
    {
        LOG_ERR("[H265FrameClassifier] Failed to allocate classifier structure\n");
        return NULL;
    }
    memset(classifier, 0, sizeof(H265FrameClassifier_t));
    
    // 设置缓冲区大小
    classifier->cacheBufSize = DEFAULT_CACHE_BUF_SIZE;
    if (combinedFrameBufSize == 0)
    {
        classifier->combinedBufSize = DEFAULT_COMBINED_BUF_SIZE;
    }
    else
    {
        classifier->combinedBufSize = combinedFrameBufSize;
    }
    
    // 分配缓存缓冲区
    classifier->vpsBuf = (unsigned char *)malloc(classifier->cacheBufSize);
    classifier->spsBuf = (unsigned char *)malloc(classifier->cacheBufSize);
    classifier->ppsBuf = (unsigned char *)malloc(classifier->cacheBufSize);
    classifier->seiBuf = (unsigned char *)malloc(classifier->cacheBufSize);
    classifier->combinedFrameBuf = (unsigned char *)malloc(classifier->combinedBufSize);
    
    if (classifier->vpsBuf == NULL || classifier->spsBuf == NULL ||
        classifier->ppsBuf == NULL || classifier->seiBuf == NULL ||
        classifier->combinedFrameBuf == NULL)
    {
        LOG_ERR("[H265FrameClassifier] Failed to allocate buffers\n");
        if (classifier->vpsBuf) free(classifier->vpsBuf);
        if (classifier->spsBuf) free(classifier->spsBuf);
        if (classifier->ppsBuf) free(classifier->ppsBuf);
        if (classifier->seiBuf) free(classifier->seiBuf);
        if (classifier->combinedFrameBuf) free(classifier->combinedFrameBuf);
        free(classifier);
        return NULL;
    }
    
    LOG_DEBUG("[H265FrameClassifier] Created classifier, cache buf: %zu, combined buf: %zu\n",
        classifier->cacheBufSize, classifier->combinedBufSize);
    
    return classifier;
}

void H265FrameClassifierDestroy(H265FrameClassifier_t *classifier)
{
    if (classifier == NULL)
    {
        return;
    }
    
    if (classifier->vpsBuf) free(classifier->vpsBuf);
    if (classifier->spsBuf) free(classifier->spsBuf);
    if (classifier->ppsBuf) free(classifier->ppsBuf);
    if (classifier->seiBuf) free(classifier->seiBuf);
    if (classifier->combinedFrameBuf) free(classifier->combinedFrameBuf);
    
    free(classifier);
    LOG_DEBUG("[H265FrameClassifier] Classifier destroyed\n");
}

int H265FrameClassifierProcess(H265FrameClassifier_t *classifier, 
                                const H265Frame_t *frame, 
                                H265ClassifiedFrame_t *output)
{
    if (classifier == NULL || frame == NULL || output == NULL)
    {
        LOG_ERR("[H265FrameClassifier] Invalid parameters\n");
        return -1;
    }
    
    // 获取NALU类型
    int naluType = H265GetNALUType(frame->data, frame->size);
    if (naluType < 0)
    {
        LOG_DEBUG("[H265FrameClassifier] Failed to get NALU type\n");
        return -1;
    }
    
    // 处理不同类型的NALU
    if (naluType == H265_NALU_TYPE_VPS)
    {
        // 缓存VPS
        if (frame->size <= classifier->cacheBufSize)
        {
            memcpy(classifier->vpsBuf, frame->data, frame->size);
            classifier->vpsLen = frame->size;
            classifier->hasVps = 1;
            LOG_DEBUG("[H265FrameClassifier] Cached VPS, length: %zu\n", frame->size);
        }
        return 1; // 需要继续处理，不输出帧
    }
    else if (naluType == H265_NALU_TYPE_SPS)
    {
        // 缓存SPS
        if (frame->size <= classifier->cacheBufSize)
        {
            memcpy(classifier->spsBuf, frame->data, frame->size);
            classifier->spsLen = frame->size;
            classifier->hasSps = 1;
            LOG_DEBUG("[H265FrameClassifier] Cached SPS, length: %zu\n", frame->size);
        }
        return 1; // 需要继续处理，不输出帧
    }
    else if (naluType == H265_NALU_TYPE_PPS)
    {
        // 缓存PPS
        if (frame->size <= classifier->cacheBufSize)
        {
            memcpy(classifier->ppsBuf, frame->data, frame->size);
            classifier->ppsLen = frame->size;
            classifier->hasPps = 1;
            LOG_DEBUG("[H265FrameClassifier] Cached PPS, length: %zu\n", frame->size);
        }
        return 1; // 需要继续处理，不输出帧
    }
    else if (naluType == H265_NALU_TYPE_SEI)
    {
        // 缓存SEI（可选）
        if (frame->size <= classifier->cacheBufSize)
        {
            memcpy(classifier->seiBuf, frame->data, frame->size);
            classifier->seiLen = frame->size;
            classifier->hasSei = 1;
            LOG_DEBUG("[H265FrameClassifier] Cached SEI, length: %zu\n", frame->size);
        }
        return 1; // 需要继续处理，不输出帧
    }
    else if (naluType == H265_NALU_TYPE_IDR_N_LP || 
             naluType == H265_NALU_TYPE_IDR_W_RADL ||
             naluType == H265_NALU_TYPE_CRA_NUT)
    {
        // IDR帧：组合VPS+SPS+PPS+SEI+IDR成完整I帧
        int totalSize = 0;
        if (classifier->hasVps) totalSize += classifier->vpsLen;
        if (classifier->hasSps) totalSize += classifier->spsLen;
        if (classifier->hasPps) totalSize += classifier->ppsLen;
        if (classifier->hasSei) totalSize += classifier->seiLen;
        totalSize += frame->size;
        
        if (totalSize > (int)classifier->combinedBufSize)
        {
            LOG_ERR("[H265FrameClassifier] Combined I frame too large: %d bytes\n", totalSize);
            return -1;
        }
        
        // 组合帧
        int offset = 0;
        if (classifier->hasVps)
        {
            memcpy(classifier->combinedFrameBuf + offset, classifier->vpsBuf, classifier->vpsLen);
            offset += classifier->vpsLen;
        }
        if (classifier->hasSps)
        {
            memcpy(classifier->combinedFrameBuf + offset, classifier->spsBuf, classifier->spsLen);
            offset += classifier->spsLen;
        }
        if (classifier->hasPps)
        {
            memcpy(classifier->combinedFrameBuf + offset, classifier->ppsBuf, classifier->ppsLen);
            offset += classifier->ppsLen;
        }
        if (classifier->hasSei)
        {
            memcpy(classifier->combinedFrameBuf + offset, classifier->seiBuf, classifier->seiLen);
            offset += classifier->seiLen;
        }
        memcpy(classifier->combinedFrameBuf + offset, frame->data, frame->size);
        offset += frame->size;
        
        // 清空SEI缓存（VPS/SPS/PPS保留，因为后续IDR可能还需要）
        classifier->hasSei = 0;
        classifier->seiLen = 0;
        
        LOG_DEBUG("[H265FrameClassifier] Combined I frame: VPS=%d, SPS=%d, PPS=%d, SEI=%d, IDR=%zu, total=%d\n",
            classifier->hasVps ? classifier->vpsLen : 0,
            classifier->hasSps ? classifier->spsLen : 0, 
            classifier->hasPps ? classifier->ppsLen : 0, 
            classifier->hasSei ? classifier->seiLen : 0, 
            frame->size, totalSize);
        
        // 输出组合后的I帧
        output->data = classifier->combinedFrameBuf;
        output->size = totalSize;
        output->type = H265_FRAME_TYPE_I;
        return 0; // 成功输出帧
    }
    else if (naluType >= H265_NALU_TYPE_TRAIL_N && naluType <= H265_NALU_TYPE_RASL_R)
    {
        // TRAIL/TSA/STSA/RADL/RASL 帧：判断是P帧还是B帧
        int sliceType = H265GetSliceType(frame->data, frame->size);
        if (sliceType == 'P')
        {
            output->data = frame->data;
            output->size = frame->size;
            output->type = H265_FRAME_TYPE_P;
            LOG_DEBUG("[H265FrameClassifier] P frame, size: %zu\n", frame->size);
            return 0; // 成功输出帧
        }
        else if (sliceType == 'B')
        {
            output->data = frame->data;
            output->size = frame->size;
            output->type = H265_FRAME_TYPE_B;
            LOG_DEBUG("[H265FrameClassifier] B frame, size: %zu\n", frame->size);
            return 0; // 成功输出帧
        }
        else
        {
            // 无法判断或I slice，默认当作P帧处理
            LOG_DEBUG("[H265FrameClassifier] Unknown slice type (%d), treating as P frame, size: %zu\n", 
                sliceType, frame->size);
            output->data = frame->data;
            output->size = frame->size;
            output->type = H265_FRAME_TYPE_P;
            return 0; // 成功输出帧
        }
    }
    else
    {
        // 其他类型的NALU（如AUD等），直接输出
        LOG_DEBUG("[H265FrameClassifier] Other NALU type: %d, size: %zu\n", naluType, frame->size);
        output->data = frame->data;
        output->size = frame->size;
        output->type = H265_FRAME_TYPE_OTHER;
        return 0; // 成功输出帧
    }
}

