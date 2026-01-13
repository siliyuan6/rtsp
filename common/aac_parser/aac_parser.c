/**
 * @file aac_parser.c
 * @brief AAC 码流解析模块实现
 */

#include "aac_parser.h"
#include "../log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @brief AAC 解析器内部结构体
 */
struct AACParser_s {
    FILE *fp;                  // 文件指针
    unsigned char *buffer;     // 读取缓冲区
    size_t bufferSize;         // 缓冲区大小
    size_t readPos;            // 读取位置
    size_t validDataSize;      // 缓冲区中有效数据大小
    int loopEn;                // 是否循环读取
};

#define DEFAULT_BUFFER_SIZE (1024 * 1024)  // 默认1MB

/**
 * @brief 查找 ADTS 同步字（0xFFF）
 * 
 * @param data 数据缓冲区
 * @param size 缓冲区大小
 * @param offset 起始偏移量
 * @return 找到同步字返回开始位置，未找到返回-1
 */
static int FindADTSSyncWord(const unsigned char *data, size_t size, int offset)
{
    if (offset < 0 || (size_t)offset >= size)
    {
        return -1;
    }

    // ADTS 同步字是 0xFFF（12 bits），位于前两个字节
    // 第一个字节：0xFF（高8位）
    // 第二个字节：0xF?（低4位为同步字的低4位）
    for (int i = offset; i < (int)size - 1; i++)
    {
        if (data[i] == 0xFF && (data[i + 1] & 0xF0) == 0xF0)
        {
            // 找到可能的同步字，验证是否符合 ADTS 格式
            // 检查第二个字节的低位（应该是 0xF?，但不应该是 0xFF）
            // 实际上，0xFF 0xF0-0xFF 都是有效的同步字开始
            return i;
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
static int ReadBufferFromFile(AACParser_t *parser)
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
                LOG_DEBUG("[AACParser] File EOF reached, restarting from beginning...\n");
                fseek(parser->fp, 0, SEEK_SET);
                clearerr(parser->fp);
                // 重新读取
                readSize = fread(parser->buffer + parser->validDataSize, 
                            1, 
                            parser->bufferSize - parser->validDataSize, 
                            parser->fp);
                if (readSize == 0)
                {
                    LOG_WARN("[AACParser] Empty file after loop restart\n");
                    return 0;
                }
                parser->validDataSize += readSize;
                LOG_DEBUG("[AACParser] Read %zu bytes after loop restart\n", readSize);
                return (int)readSize;
            }
            else
            {
                // 不循环：返回EOF
                LOG_DEBUG("[AACParser] File EOF reached (no loop)\n");
                return 0;
            }
        }
        else
        {
            // 读取错误
            LOG_ERR("[AACParser] File read error: %d\n", ferror(parser->fp));
            return -1;
        }
    }
    
    parser->validDataSize += readSize;
    LOG_DEBUG("[AACParser] Read %zu bytes from file\n", readSize);
    
    return (int)readSize;
}

AACParser_t* AACParserCreate(const char *filePath, size_t readBufSize, int loopEn)
{
    if (filePath == NULL)
    {
        LOG_ERR("[AACParser] File path is NULL\n");
        return NULL;
    }

    // 分配解析器结构体
    AACParser_t *parser = (AACParser_t *)malloc(sizeof(AACParser_t));
    if (parser == NULL)
    {
        LOG_ERR("[AACParser] Failed to allocate parser structure\n");
        return NULL;
    }
    memset(parser, 0, sizeof(AACParser_t));

    // 打开文件
    parser->fp = fopen(filePath, "rb");
    if (parser->fp == NULL)
    {
        LOG_ERR("[AACParser] Failed to open file: %s\n", filePath);
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
        LOG_ERR("[AACParser] Failed to allocate buffer\n");
        fclose(parser->fp);
        free(parser);
        return NULL;
    }
    memset(parser->buffer, 0, parser->bufferSize);

    // 初始化状态
    parser->validDataSize = 0;
    parser->loopEn = loopEn ? 1 : 0;

    LOG_INFO("[AACParser] Created parser for file: %s, buffer size: %zu, loop: %d\n",
        filePath, parser->bufferSize, parser->loopEn);

    return parser;
}

void AACParserDestroy(AACParser_t *parser)
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
    LOG_DEBUG("[AACParser] Parser destroyed\n");
}

int AACParseADTSHeader(const unsigned char *data, int len, ADTSHeaderInfo_t *headerInfo)
{
    if (data == NULL || len < 7 || headerInfo == NULL)
    {
        return -1;
    }

    // 验证同步字（前12位应该是0xFFF）
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0)
    {
        return -1;
    }

    // 解析 ADTS 固定头部（前7字节）
    // 根据 ISO/IEC 13818-7 (MPEG-2 AAC)
    // Byte 0: syncword (8 bits, high) = 0xFF
    // Byte 1: [syncword (4 bits, low) = 0xF | MPEG version (1) | Layer (2) | Protection absent (1) | Profile (2) | Sampling frequency index (2 bits, high)]
    // Byte 2: [Sampling frequency index (2 bits, low) | Private bit (1) | Channel configuration (3) | Original/copy (1) | Home (1) | Copyright ID bit (1) | Copyright ID start (1)]
    // Byte 3: [Frame length (13 bits, high 2 bits) | Buffer fullness (11 bits, high 3 bits)]
    // Byte 4: [Frame length (13 bits, middle 8 bits)]
    // Byte 5: [Frame length (13 bits, low 3 bits) | Buffer fullness (11 bits, middle 8 bits)]
    // Byte 6: [Buffer fullness (11 bits, low 3 bits) | Number of raw data blocks (2 bits)]
    // Byte 7-8: CRC (16 bits, only if protection_absent == 0)

    unsigned char byte0 = data[0];
    unsigned char byte1 = data[1];
    unsigned char byte2 = data[2];
    unsigned char byte3 = data[3];
    unsigned char byte4 = data[4];
    unsigned char byte5 = data[5];

    (void)byte0; // 已验证同步字，无需存储
    
    // Protection absent (1 bit): byte1 bit 2
    headerInfo->protectionAbsent = (byte1 >> 2) & 0x01;

    // Profile (2 bits): byte1 bits 6-5
    headerInfo->profile = (byte1 >> 6) & 0x03;

    // Sampling frequency index (4 bits): byte1 bits 1-0 (high 2) | byte2 bits 7-6 (low 2)
    headerInfo->samplingFrequencyIndex = ((byte1 & 0x03) << 2) | ((byte2 >> 6) & 0x03);

    // Channel configuration (3 bits): byte2 bits 5-3
    headerInfo->channelConfig = (byte2 >> 3) & 0x07;

    // Original/copy (1 bit)
    headerInfo->original = (byte2 >> 2) & 0x01;

    // Home (1 bit)
    headerInfo->home = (byte2 >> 1) & 0x01;

    // Frame length (13 bits): byte3[5:0] + byte4[7:0] + byte5[7:5]
    int frameLength = ((int)(byte3 & 0x03) << 11) | ((int)byte4 << 3) | ((byte5 >> 5) & 0x07);
    headerInfo->frameLength = frameLength;

    // ADTS 头部长度：protection_absent == 1 时 7 字节，否则 9 字节（包含 CRC）
    int headerLength = headerInfo->protectionAbsent ? 7 : 9;

    // 检查是否有足够的空间存放完整头部
    if (len < headerLength)
    {
        return -1;
    }

    return headerLength;
}

int AACParserNextFrame(AACParser_t *parser, AACFrame_t *frame)
{
    if (parser == NULL || frame == NULL)
    {
        LOG_ERR("[AACParser] Invalid parameters\n");
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

    // 步骤1: 找到第一个 ADTS 同步字
    // 注意：如果 readPos > 0，说明之前已经处理过一些数据，应该从 readPos 开始查找
    // 但如果 readPos == 0，说明这是第一次查找或之前的数据已经被处理完，从 0 开始查找
    int firstSyncPos = -1;
    
    // 从 readPos 开始查找（正常情况：readPos 指向下一帧应该开始的位置）
    if (parser->readPos < (int)parser->validDataSize)
    {
        firstSyncPos = FindADTSSyncWord(parser->buffer, parser->validDataSize, parser->readPos);
    }
    
    // 如果从 readPos 开始没找到，且 readPos == 0，说明缓冲区中可能没有有效的同步字
    // 需要读取更多数据或移动数据
    
    if (firstSyncPos < 0)
    {
        // 没找到同步字，尝试读取更多数据
        if (parser->readPos > 0)
        {
            // 移动剩余数据到开头
            int remainingSize = parser->validDataSize - parser->readPos;
            if (remainingSize > 0)
            {
                memmove(parser->buffer, parser->buffer + parser->readPos, remainingSize);
                parser->validDataSize = remainingSize;
                parser->readPos = 0;
                
                // 重新查找
                firstSyncPos = FindADTSSyncWord(parser->buffer, parser->validDataSize, 0);
                if (firstSyncPos < 0)
                {
                    // 仍然找不到，尝试读取更多数据
                    int read_result = ReadBufferFromFile(parser);
                    if (read_result <= 0)
                    {
                        if (read_result == 0 && feof(parser->fp) && !parser->loopEn)
                        {
                            // EOF
                            return 1;
                        }
                        // 读取错误或循环重启中，继续尝试
                        return -1;
                    }
                    // 再次查找
                    firstSyncPos = FindADTSSyncWord(parser->buffer, parser->validDataSize, 0);
                }
            }
            else
            {
                // 没有剩余数据，重置并读取
                parser->validDataSize = 0;
                parser->readPos = 0;
                int read_result = ReadBufferFromFile(parser);
                if (read_result <= 0)
                {
                    if (read_result == 0 && feof(parser->fp) && !parser->loopEn)
                    {
                        return 1; // EOF
                    }
                    return -1;
                }
                firstSyncPos = FindADTSSyncWord(parser->buffer, parser->validDataSize, 0);
            }
        }
        else
        {
            // readPos == 0，尝试读取更多数据
            int read_result = ReadBufferFromFile(parser);
            if (read_result <= 0)
            {
                if (read_result == 0 && feof(parser->fp) && !parser->loopEn)
                {
                    return 1; // EOF
                }
                return -1;
            }
            firstSyncPos = FindADTSSyncWord(parser->buffer, parser->validDataSize, 0);
        }
        
        if (firstSyncPos < 0)
        {
            LOG_ERR("[AACParser] No ADTS sync word found, validDataSize=%zu, readPos=%zu\n", 
                parser->validDataSize, parser->readPos);
            
            // 检查是否是文件结束
            if (parser->validDataSize == 0 && feof(parser->fp) && !parser->loopEn)
            {
                return 1; // EOF
            }
            return -1;
        }
    }

    // 解析第一个 ADTS 帧头部，获取帧长度
    ADTSHeaderInfo_t headerInfo;
    
    // 调试日志：记录同步字位置和缓冲区状态
    static int first_sync_logged = 0;
    if (!first_sync_logged && firstSyncPos == 0)
    {
        LOG_INFO("[AACParser] First sync word found at position %d, buffer[0:7]=%02x %02x %02x %02x %02x %02x %02x, validDataSize=%zu\n",
            firstSyncPos,
            parser->buffer[0], parser->buffer[1], parser->buffer[2], parser->buffer[3],
            parser->buffer[4], parser->buffer[5], parser->buffer[6],
            parser->validDataSize);
        first_sync_logged = 1;
    }
    
    int headerLen = AACParseADTSHeader(parser->buffer + firstSyncPos, 
                                       parser->validDataSize - firstSyncPos, 
                                       &headerInfo);
    if (headerLen < 0)
    {
        LOG_ERR("[AACParser] Failed to parse ADTS header at position %d\n", firstSyncPos);
        // 跳过这个无效的同步字，继续查找下一个
        parser->readPos = firstSyncPos + 1;
        return -1;
    }

    // 检查帧长度是否合理（至少应该包含头部）
    if (headerInfo.frameLength < headerLen || headerInfo.frameLength > 8191)
    {
        LOG_ERR("[AACParser] Invalid frame length: %d (headerLen=%d) at position %d\n", 
            headerInfo.frameLength, headerLen, firstSyncPos);
        // 跳过这个无效的同步字，继续查找下一个
        parser->readPos = firstSyncPos + 1;
        return -1;
    }

    // 检查是否有足够的数据包含完整帧
    int frameEndPos = firstSyncPos + headerInfo.frameLength;
    if (frameEndPos > (int)parser->validDataSize)
    {
        // 帧数据不完整，需要读取更多数据
        // 先将当前数据移动到开头（如果有需要的话）
        // 注意：如果 firstSyncPos > 0，说明文件开头有非 ADTS 数据，这些数据需要保留在缓冲区中
        // 但是，为了正确读取完整的帧，我们需要将帧的开始位置移动到缓冲区开头
        if (firstSyncPos > 0)
        {
            int remainingSize = parser->validDataSize - firstSyncPos;
            if (remainingSize > 0)
            {
                // 将帧数据（从 firstSyncPos 开始）移动到缓冲区开头
                // 注意：这会丢失 firstSyncPos 之前的数据，但这是必要的，因为
                // 我们需要确保帧数据从缓冲区开头开始，以便后续能正确读取完整帧
                memmove(parser->buffer, parser->buffer + firstSyncPos, remainingSize);
                parser->validDataSize = remainingSize;
                parser->readPos = 0;
                firstSyncPos = 0;
                frameEndPos = headerInfo.frameLength;
            }
        }

        // 尝试读取更多数据，直到有足够的帧数据
        while (parser->validDataSize < (size_t)headerInfo.frameLength)
        {
            int read_result = ReadBufferFromFile(parser);
            if (read_result <= 0)
            {
                if (read_result == 0 && feof(parser->fp) && !parser->loopEn)
                {
                    // EOF，数据不完整
                    LOG_ERR("[AACParser] Incomplete frame at EOF: need %d, have %zu\n",
                        headerInfo.frameLength, parser->validDataSize);
                    return 1; // EOF
                }
                else if (read_result < 0)
                {
                    // 读取错误
                    return -1;
                }
                // read_result == 0 但可能是循环重启，继续等待
            }
            
            // 如果读取后仍然不够，且文件已结束，返回错误
            if (parser->validDataSize < (size_t)headerInfo.frameLength && 
                feof(parser->fp) && !parser->loopEn)
            {
                LOG_ERR("[AACParser] Incomplete frame: need %d, have %zu\n",
                    headerInfo.frameLength, parser->validDataSize);
                return 1; // EOF
            }
        }
    }

    // 设置帧数据（确保 firstSyncPos 在数据移动后已经更新为 0）
    frame->data = parser->buffer + firstSyncPos;
    frame->size = headerInfo.frameLength;
    frame->headerInfo = headerInfo;

    // 调试日志：记录第一个帧的信息
    static int first_frame_logged = 0;
    if (!first_frame_logged)
    {
        LOG_INFO("[AACParser] First frame: firstSyncPos=%d, frameLength=%d, readPos=%zu, validDataSize=%zu\n",
            firstSyncPos, headerInfo.frameLength, parser->readPos, parser->validDataSize);
        LOG_INFO("[AACParser] First frame data[0:7]=%02x %02x %02x %02x %02x %02x %02x\n",
            frame->data[0], frame->data[1], frame->data[2], frame->data[3],
            frame->data[4], frame->data[5], frame->data[6]);
        LOG_INFO("[AACParser] Buffer[0:7]=%02x %02x %02x %02x %02x %02x %02x\n",
            parser->buffer[0], parser->buffer[1], parser->buffer[2], parser->buffer[3],
            parser->buffer[4], parser->buffer[5], parser->buffer[6]);
        first_frame_logged = 1;
    }

    // 更新 readPos 为下一帧的开始位置
    // 如果数据被移动过，firstSyncPos 已经是 0，所以这里是正确的
    parser->readPos = firstSyncPos + headerInfo.frameLength;

    // 检查是否需要移动数据到开头（当 readPos 超过缓冲区一半时）
    if (parser->readPos > parser->bufferSize / 2)
    {
        // 将剩余数据移到缓冲区开头
        size_t remainingSize = parser->validDataSize - parser->readPos;
        if (remainingSize > 0 && remainingSize <= parser->bufferSize)
        {
            // 检查是否会覆盖当前帧数据
            // 当前帧数据从 firstSyncPos 开始，长度为 headerInfo.frameLength
            // 剩余数据从 readPos 开始，长度为 remainingSize
            // 如果 firstSyncPos + headerInfo.frameLength > remainingSize，说明帧数据可能被覆盖
            // 但实际上，frame->data 已经被返回给调用者，调用者应该已经拷贝了数据
            // 所以这里可以安全地移动数据
            // 但为了安全，我们检查帧数据的结束位置是否在剩余数据之前
            int frameEnd = firstSyncPos + headerInfo.frameLength;
            if (frameEnd < (int)parser->readPos)
            {
                // 帧数据已经完全处理，可以安全移动
                memmove(parser->buffer, parser->buffer + parser->readPos, remainingSize);
                parser->validDataSize = remainingSize;
                parser->readPos = 0;
                
                LOG_DEBUG("[AACParser] Moved %zu bytes to buffer start\n", remainingSize);
            }
            else
            {
                // 帧数据的结束位置等于或大于 readPos，不应该发生这种情况
                // 但为了安全，不执行 memmove
                LOG_DEBUG("[AACParser] Skipping memmove: frameEnd=%d, readPos=%zu\n",
                    frameEnd, parser->readPos);
            }
        }
        else if (remainingSize == 0)
        {
            // 没有剩余数据，重置缓冲区
            parser->validDataSize = 0;
            parser->readPos = 0;
        }
        else
        {
            // remainingSize 异常，重置缓冲区
            LOG_DEBUG("[AACParser] Invalid remainingSize=%zu, resetting buffer\n", remainingSize);
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

