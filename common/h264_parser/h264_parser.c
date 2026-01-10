/**
 * @file h264_parser.c
 * @brief H264 码流解析模块实现
 */

#include "h264_parser.h"
#include "../log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief H264 解析器内部结构体
 */
struct H264Parser_s {
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
    
    // 检查是否有足够的空间存放 NALU 头
    if (nalu_header_pos >= (int)size)
    {
        return 0;
    }
    
    // 获取 NALU 头字节
    unsigned char nalu_header = data[nalu_header_pos];
    
    // 提取 NALU 类型（低5位）
    int nalu_type = nalu_header & 0x1F;
    
    // 验证 NALU 类型是否在有效范围内（0-31）
    if (nalu_type < 0 || nalu_type > 31)
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
        
        // 检查起始码前是否有足够的数据长度
        if (startcode_pos > 0 && startcode_pos < 100)
        {
            // 检查起始码前是否有非0x00的数据
            int non_zero_count = 0;
            int check_start = (startcode_pos > 20) ? (startcode_pos - 20) : 0;
            for (int i = check_start; i < startcode_pos; i++)
            {
                if (data[i] != 0x00)
                {
                    non_zero_count++;
                }
            }
            
            // 如果起始码前20字节内几乎没有非0x00数据，可能是帧内的起始码模式
            if (non_zero_count < 3)
            {
                return 0;
            }
        }
    }
    
    return 1; // 通过所有验证，是有效的起始码
}

/**
 * @brief 查找 H264 起始码
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
 * @brief 从文件读取1MB数据到缓冲区
 * 
 * @param parser 解析器指针
 * @return 成功返回读取的字节数，EOF返回0，错误返回-1
 */
static int ReadBufferFromFile(H264Parser_t *parser)
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
                LOG_DEBUG("[H264Parser] File EOF reached, restarting from beginning...\n");
                fseek(parser->fp, 0, SEEK_SET);
                clearerr(parser->fp);
                // 重新读取
                readSize = fread(parser->buffer + parser->validDataSize, 
                            1, 
                            parser->bufferSize - parser->validDataSize, 
                            parser->fp);
                if (readSize == 0)
                {
                    LOG_WARN("[H264Parser] Empty file after loop restart\n");
                    return 0;
                }
                parser->validDataSize += readSize;
                LOG_DEBUG("[H264Parser] Read %zu bytes after loop restart\n", readSize);
                return (int)readSize;
            }
            else
            {
                // 不循环：返回EOF
                LOG_DEBUG("[H264Parser] File EOF reached (no loop)\n");
                return 0;
            }
        }
        else
        {
            // 读取错误
            LOG_ERR("[H264Parser] File read error: %d\n", ferror(parser->fp));
            return -1;
        }
    }
    
    parser->validDataSize += readSize;
    LOG_DEBUG("[H264Parser] Read %zu bytes from file\n", readSize);
    
    return (int)readSize;
}

H264Parser_t* H264ParserCreate(const char *filePath, size_t readBufSize, int loopEn)
{
    if (filePath == NULL)
    {
        LOG_ERR("[H264Parser] File path is NULL\n");
        return NULL;
    }

    // 分配解析器结构体
    H264Parser_t *parser = (H264Parser_t *)malloc(sizeof(H264Parser_t));
    if (parser == NULL)
    {
        LOG_ERR("[H264Parser] Failed to allocate parser structure\n");
        return NULL;
    }
    memset(parser, 0, sizeof(H264Parser_t));

    // 打开文件
    parser->fp = fopen(filePath, "rb");
    if (parser->fp == NULL)
    {
        LOG_ERR("[H264Parser] Failed to open file: %s\n", filePath);
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
        LOG_ERR("[H264Parser] Failed to allocate buffer\n");
        fclose(parser->fp);
        free(parser);
        return NULL;
    }
    memset(parser->buffer, 0, parser->bufferSize);

    // 初始化状态
    parser->validDataSize = 0;
    parser->loopEn = loopEn ? 1 : 0;

    LOG_INFO("[H264Parser] Created parser for file: %s, buffer size: %zu, loop: %d\n",
        filePath, parser->bufferSize, parser->loopEn);

    return parser;
}

void H264ParserDestroy(H264Parser_t *parser)
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
    LOG_DEBUG("[H264Parser] Parser destroyed\n");
}

int H264ParserNextFrame(H264Parser_t *parser, H264Frame_t *frame)
{
    if (parser == NULL || frame == NULL)
    {
        LOG_ERR("[H264Parser] Invalid parameters\n");
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
    // LOG_DEBUG("[H264Parser] readPos=%zu, validDataSize=%zu\n", parser->readPos, parser->validDataSize);
    int firstFrameStartCodeLen = 0;
    int firstFramePos = FindStartCode(parser->buffer, // 数据缓冲区地址
                                     parser->validDataSize, // 有效数据大小
                                     parser->readPos, // 查找起始位置
                                     &firstFrameStartCodeLen);
    if (firstFramePos < 0)
    {
        LOG_ERR("[H264Parser] First frame: no start code found, validDataSize=%zu, readPos=%zu\n", 
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
            LOG_ERR("[H264Parser] No remaining data after first start code\n");
            return -1;
        }

        // 将第一个起始码及其后面的数据移动到缓冲区开头
        memmove(parser->buffer, 
                parser->buffer + firstFramePos, 
                remainingSize);
        parser->readPos = 0;
        parser->validDataSize = remainingSize;
        LOG_DEBUG("[H264Parser] Moved %d bytes to buffer start, continuing to read...\n", remainingSize);
        
        // 尝试读取更多数据
        int read_result = ReadBufferFromFile(parser);
        if (read_result < 0)
        {
            // 读取错误
            LOG_ERR("[H264Parser] Error reading file after moving data\n");
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
                    LOG_DEBUG("[H264Parser] Last frame from EOF (no start code found): size=%zu\n", frame->size);
                    return 0;
                }
            }
            // 如果找不到第一个起始码，说明剩余数据不完整，返回EOF
            LOG_DEBUG("[H264Parser] No first start code found after moving data, EOF\n");
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
                    LOG_DEBUG("[H264Parser] Last frame from EOF: size=%d\n", lastFrameSize);
                    return 0;
                }
            }
            LOG_ERR("[H264Parser] No second start code found after reading more data, read_result=%d, eof=%d, loopEn=%d\n",
                read_result, feof(parser->fp) ? 1 : 0, parser->loopEn);
            return -1;
        }
    }

    // 步骤3: 两个起始码中间区域就是该帧
    int frameSize = secondFramePos - firstFramePos;
    if (frameSize <= 0)
    {
        LOG_ERR("[H264Parser] Invalid frame size: %d\n", frameSize);
        return -1;
    }
    
    // 先设置帧数据指针和大小（在 memmove 之前设置，确保指向正确的数据）
    frame->data = parser->buffer + firstFramePos;
    frame->size = frameSize;
    frame->startCodeLen = firstFrameStartCodeLen;
    
    // 更新 readPos 为第二个起始码位置，下次从这里开始查找
    parser->readPos = secondFramePos;
    
    // 检查是否需要移动数据到开头（当 readPos 超过缓冲区一半时）
    // 注意：frame->data 指向内部缓冲区，用户需要立即拷贝数据
    // 如果 memmove 的目标区域会覆盖帧数据，需要延迟执行或使用临时缓冲区
    if (parser->readPos > parser->bufferSize / 2)
    {
        // 将第二个起始码之后的数据移到缓冲区开头
        size_t remainingSize = parser->validDataSize - parser->readPos;
        if (remainingSize > 0 && remainingSize <= parser->bufferSize)
        {
            // 检查帧数据是否会被 memmove 的目标区域覆盖
            // memmove 的目标区域是 [0, remainingSize)
            // 帧数据区域是 [firstFramePos, secondFramePos)
            // 如果 firstFramePos < remainingSize，目标区域会覆盖帧数据的开始部分
            if (firstFramePos < (int)remainingSize)
            {
                // 帧数据会被覆盖，不执行 memmove
                // 注意：frame->data 指向内部缓冲区，用户必须立即拷贝数据（按照 API 约定）
                // 由于不执行 memmove，validDataSize 保持原值
                // readPos 已经更新为 secondFramePos，下次调用时会从 secondFramePos 开始查找
                // ReadBufferFromFile 会在 validDataSize 之后追加数据（有溢出检查）
                LOG_DEBUG("[H264Parser] Skipping memmove to avoid overwriting frame data: firstFramePos=%d, remainingSize=%zu\n",
                    firstFramePos, remainingSize);
                // 注意：validDataSize 保持不变，但下次查找时会从 readPos 开始，所以不会访问已处理的数据
            }
            else
            {
                // 帧数据不会被覆盖，可以安全执行 memmove
                memmove(parser->buffer, parser->buffer + parser->readPos, remainingSize);
                parser->validDataSize = remainingSize;
                parser->readPos = 0;
                
                LOG_DEBUG("[H264Parser] Moved %zu bytes to buffer start\n", remainingSize);
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
 * @brief 获取H.264起始码长度
 * 
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 起始码长度（3或4），如果没有起始码返回0
 */
int H264GetStartCodeLen(const unsigned char *data, int len)
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
 * @param data H.264数据（包含起始码）
 * @param len 数据长度
 * @return NALU类型，失败返回-1
 */
int H264GetNALUType(const unsigned char *data, int len)
{
	int startCodeLen = H264GetStartCodeLen(data, len);
	if (startCodeLen <= 0 || startCodeLen >= len)
	{
		return -1;
	}
	
	// NALU类型在起始码后的第一个字节的低5位
	return data[startCodeLen] & 0x1F;
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
 * @param data NALU数据（包含起始码）
 * @param len 数据长度
 * @return 'I'表示I slice，'P'表示P slice，'B'表示B slice，失败返回-1
 */
int H264GetSliceType(const unsigned char *data, int len)
{
	int startCodeLen = H264GetStartCodeLen(data, len);
	if (startCodeLen <= 0 || startCodeLen >= len)
	{
		return -1;
	}
	
	// NALU头在起始码后
	int naluHeaderPos = startCodeLen;
	if (naluHeaderPos >= len)
	{
		return -1;
	}
	
	unsigned char naluHeader = data[naluHeaderPos];
	int naluType = naluHeader & 0x1F;
	
	// IDR帧的NALU type是5，直接返回I
	if (naluType == H264_NALU_TYPE_IDR)
	{
		return 'I';
	}
	
	// 只有NON_IDR帧（type=1）需要解析slice header
	if (naluType != H264_NALU_TYPE_NON_IDR)
	{
		return -1;
	}
	
	// slice header从NALU payload开始（跳过NALU头1字节）
	int sliceHeaderStart = naluHeaderPos + 1;
	if (sliceHeaderStart >= len)
	{
		return -1;
	}
	
	// 解析slice header
	// 需要跳过：first_mb_in_slice (ue(v))
	// 然后读取：slice_type (ue(v))
	// 注意：exp-Golomb是按位编码的，需要按位读取
	int bitOffset = sliceHeaderStart * 8; // 转换为位偏移量
	
	// 跳过first_mb_in_slice (exp-Golomb编码)
	int firstMb = ReadExpGolomb(data, len, &bitOffset);
	if (firstMb < 0)
	{
		// 解析失败，默认当作P帧处理
		LOG_DEBUG("[H264GetSliceType] Failed to parse first_mb_in_slice, defaulting to P frame\n");
		return 'P';
	}
	
	// 读取slice_type (exp-Golomb编码)
	int sliceType = ReadExpGolomb(data, len, &bitOffset);
	if (sliceType < 0)
	{
		// 解析失败，默认当作P帧处理
		LOG_DEBUG("[H264GetSliceType] Failed to parse slice_type, defaulting to P frame\n");
		return 'P';
	}
	
	// slice_type值映射：
	// 0, 3, 5, 8: I slice
	// 1, 6: P slice
	// 2, 4, 7, 9: B slice
	if (sliceType == 0 || sliceType == 3 || sliceType == 5 || sliceType == 8)
	{
		return 'I';
	}
	else if (sliceType == 1 || sliceType == 6)
	{
		return 'P';
	}
	else if (sliceType == 2 || sliceType == 4 || sliceType == 7 || sliceType == 9)
	{
		return 'B';
	}
	
	// 未知的slice_type，默认当作P帧
	LOG_DEBUG("[H264GetSliceType] Unknown slice_type: %d, defaulting to P frame\n", sliceType);
	return 'P';
}

/**
 * @brief H.264帧分类器内部结构体
 */
struct H264FrameClassifier_s {
	unsigned char *spsBuf;          // SPS缓存缓冲区
	unsigned char *ppsBuf;          // PPS缓存缓冲区
	unsigned char *seiBuf;          // SEI缓存缓冲区
	unsigned char *combinedFrameBuf; // 组合帧缓冲区
	int spsLen;                     // SPS长度
	int ppsLen;                     // PPS长度
	int seiLen;                     // SEI长度
	int hasSps;                     // 是否有SPS
	int hasPps;                     // 是否有PPS
	int hasSei;                     // 是否有SEI
	size_t cacheBufSize;            // 缓存缓冲区大小
	size_t combinedBufSize;         // 组合缓冲区大小
};

#define DEFAULT_CACHE_BUF_SIZE (2 * 1024 * 1024)  // 默认2MB
#define DEFAULT_COMBINED_BUF_SIZE (4 * 1024 * 1024) // 默认4MB

H264FrameClassifier_t* H264FrameClassifierCreate(size_t combinedFrameBufSize)
{
	H264FrameClassifier_t *classifier = (H264FrameClassifier_t *)malloc(sizeof(H264FrameClassifier_t));
	if (classifier == NULL)
	{
		LOG_ERR("[H264FrameClassifier] Failed to allocate classifier structure\n");
		return NULL;
	}
	memset(classifier, 0, sizeof(H264FrameClassifier_t));
	
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
	classifier->spsBuf = (unsigned char *)malloc(classifier->cacheBufSize);
	classifier->ppsBuf = (unsigned char *)malloc(classifier->cacheBufSize);
	classifier->seiBuf = (unsigned char *)malloc(classifier->cacheBufSize);
	classifier->combinedFrameBuf = (unsigned char *)malloc(classifier->combinedBufSize);
	
	if (classifier->spsBuf == NULL || classifier->ppsBuf == NULL ||
		classifier->seiBuf == NULL || classifier->combinedFrameBuf == NULL)
	{
		LOG_ERR("[H264FrameClassifier] Failed to allocate buffers\n");
		if (classifier->spsBuf) free(classifier->spsBuf);
		if (classifier->ppsBuf) free(classifier->ppsBuf);
		if (classifier->seiBuf) free(classifier->seiBuf);
		if (classifier->combinedFrameBuf) free(classifier->combinedFrameBuf);
		free(classifier);
		return NULL;
	}
	
	LOG_DEBUG("[H264FrameClassifier] Created classifier, cache buf: %zu, combined buf: %zu\n",
		classifier->cacheBufSize, classifier->combinedBufSize);
	
	return classifier;
}

void H264FrameClassifierDestroy(H264FrameClassifier_t *classifier)
{
	if (classifier == NULL)
	{
		return;
	}
	
	if (classifier->spsBuf) free(classifier->spsBuf);
	if (classifier->ppsBuf) free(classifier->ppsBuf);
	if (classifier->seiBuf) free(classifier->seiBuf);
	if (classifier->combinedFrameBuf) free(classifier->combinedFrameBuf);
	
	free(classifier);
	LOG_DEBUG("[H264FrameClassifier] Classifier destroyed\n");
}

int H264FrameClassifierProcess(H264FrameClassifier_t *classifier, 
                                const H264Frame_t *frame, 
                                H264ClassifiedFrame_t *output)
{
	if (classifier == NULL || frame == NULL || output == NULL)
	{
		LOG_ERR("[H264FrameClassifier] Invalid parameters\n");
		return -1;
	}
	
	// 获取NALU类型
	int naluType = H264GetNALUType(frame->data, frame->size);
	if (naluType < 0)
	{
		LOG_DEBUG("[H264FrameClassifier] Failed to get NALU type\n");
		return -1;
	}
	
	// 处理不同类型的NALU
	if (naluType == H264_NALU_TYPE_SPS)
	{
		// 缓存SPS
		if (frame->size <= classifier->cacheBufSize)
		{
			memcpy(classifier->spsBuf, frame->data, frame->size);
			classifier->spsLen = frame->size;
			classifier->hasSps = 1;
			LOG_DEBUG("[H264FrameClassifier] Cached SPS, length: %zu\n", frame->size);
		}
		return 1; // 需要继续处理，不输出帧
	}
	else if (naluType == H264_NALU_TYPE_PPS)
	{
		// 缓存PPS
		if (frame->size <= classifier->cacheBufSize)
		{
			memcpy(classifier->ppsBuf, frame->data, frame->size);
			classifier->ppsLen = frame->size;
			classifier->hasPps = 1;
			LOG_DEBUG("[H264FrameClassifier] Cached PPS, length: %zu\n", frame->size);
		}
		return 1; // 需要继续处理，不输出帧
	}
	else if (naluType == H264_NALU_TYPE_SEI)
	{
		// 缓存SEI（可选）
		if (frame->size <= classifier->cacheBufSize)
		{
			memcpy(classifier->seiBuf, frame->data, frame->size);
			classifier->seiLen = frame->size;
			classifier->hasSei = 1;
			LOG_DEBUG("[H264FrameClassifier] Cached SEI, length: %zu\n", frame->size);
		}
		return 1; // 需要继续处理，不输出帧
	}
	else if (naluType == H264_NALU_TYPE_IDR)
	{
		// IDR帧：组合SPS+PPS+SEI+IDR成完整I帧
		int totalSize = 0;
		if (classifier->hasSps) totalSize += classifier->spsLen;
		if (classifier->hasPps) totalSize += classifier->ppsLen;
		if (classifier->hasSei) totalSize += classifier->seiLen;
		totalSize += frame->size;
		
		if (totalSize > (int)classifier->combinedBufSize)
		{
			LOG_ERR("[H264FrameClassifier] Combined I frame too large: %d bytes\n", totalSize);
			return -1;
		}
		
		// 组合帧
		int offset = 0;
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
		
		// 清空SEI缓存（SPS/PPS保留，因为后续IDR可能还需要）
		classifier->hasSei = 0;
		classifier->seiLen = 0;
		
		LOG_DEBUG("[H264FrameClassifier] Combined I frame: SPS=%d, PPS=%d, SEI=%d, IDR=%zu, total=%d\n",
			classifier->hasSps ? classifier->spsLen : 0, 
			classifier->hasPps ? classifier->ppsLen : 0, 
			classifier->hasSei ? classifier->seiLen : 0, 
			frame->size, totalSize);
		
		// 输出组合后的I帧
		output->data = classifier->combinedFrameBuf;
		output->size = totalSize;
		output->type = H264_FRAME_TYPE_I;
		return 0; // 成功输出帧
	}
	else if (naluType == H264_NALU_TYPE_NON_IDR)
	{
		// NON_IDR帧：判断是P帧还是B帧
		int sliceType = H264GetSliceType(frame->data, frame->size);
		if (sliceType == 'P')
		{
			output->data = frame->data;
			output->size = frame->size;
			output->type = H264_FRAME_TYPE_P;
			LOG_DEBUG("[H264FrameClassifier] P frame, size: %zu\n", frame->size);
			return 0; // 成功输出帧
		}
		else if (sliceType == 'B')
		{
			output->data = frame->data;
			output->size = frame->size;
			output->type = H264_FRAME_TYPE_B;
			LOG_DEBUG("[H264FrameClassifier] B frame, size: %zu\n", frame->size);
			return 0; // 成功输出帧
		}
		else
		{
			// 无法判断或I slice，默认当作P帧处理
			LOG_DEBUG("[H264FrameClassifier] Unknown slice type (%d), treating as P frame, size: %zu\n", 
				sliceType, frame->size);
			output->data = frame->data;
			output->size = frame->size;
			output->type = H264_FRAME_TYPE_P;
			return 0; // 成功输出帧
		}
	}
	else
	{
		// 其他类型的NALU（如AUD等），直接输出
		LOG_DEBUG("[H264FrameClassifier] Other NALU type: %d, size: %zu\n", naluType, frame->size);
		output->data = frame->data;
		output->size = frame->size;
		output->type = H264_FRAME_TYPE_OTHER;
		return 0; // 成功输出帧
	}
}
