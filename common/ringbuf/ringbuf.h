/**
 * @file ringbuf.h
 * @brief 循环队列（环形缓冲区）模块接口
 * 
 * 提供线程安全的循环队列实现，支持多生产者多消费者模式
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 循环队列结构体
 */
typedef struct RingBuffer_s
{
    unsigned char* buffer;      // 数据缓冲区
    size_t capacity;            // 缓冲区容量（字节）
    size_t readPos;             // 读位置
    size_t writePos;            // 写位置
    size_t dataSize;            // 当前数据大小
    pthread_mutex_t mutex;      // 互斥锁
    pthread_cond_t condRead;    // 读条件变量（有数据可读）
    pthread_cond_t condWrite;    // 写条件变量（有空间可写）
    int isClosed;               // 缓冲区是否已关闭
} RingBuffer_t;

/**
 * @brief 创建循环队列
 * @param capacity 缓冲区容量（字节）
 * @return 成功返回循环队列指针，失败返回NULL
 */
RingBuffer_t* RingBufferCreate(size_t capacity);

/**
 * @brief 销毁循环队列
 * @param rb 循环队列指针
 */
void RingBufferDestroy(RingBuffer_t* rb);

/**
 * @brief 写入数据到循环队列（阻塞模式）
 * @param rb 循环队列指针
 * @param data 要写入的数据
 * @param size 数据大小（字节）
 * @return 成功返回0，失败返回-1（缓冲区已关闭）
 * 
 * @note 如果缓冲区空间不足，会阻塞等待直到有足够空间
 */
int RingBufferPush(RingBuffer_t* rb, const void* data, size_t size);

/**
 * @brief 从循环队列读取数据（阻塞模式，支持超时）
 * @param rb 循环队列指针
 * @param data 读取数据的缓冲区
 * @param maxSize 缓冲区最大容量
 * @param actualSize 实际读取的数据大小（输出参数）
 * @param timeoutMs 超时时间（毫秒），-1表示无限等待，0表示非阻塞
 * @return 成功返回0，超时返回1，缓冲区已关闭返回-1
 */
int RingBufferPop(RingBuffer_t* rb, void* data, size_t maxSize, 
                  size_t* actualSize, int timeoutMs);

/**
 * @brief 获取当前数据大小
 * @param rb 循环队列指针
 * @return 当前数据大小（字节）
 */
size_t RingBufferGetSize(RingBuffer_t* rb);

/**
 * @brief 获取空闲空间大小
 * @param rb 循环队列指针
 * @return 空闲空间大小（字节）
 */
size_t RingBufferGetFree(RingBuffer_t* rb);

/**
 * @brief 关闭缓冲区
 * @param rb 循环队列指针
 * 
 * @note 关闭后，所有等待的读写操作会立即返回
 */
void RingBufferClose(RingBuffer_t* rb);

/**
 * @brief 清空缓冲区
 * @param rb 循环队列指针
 */
void RingBufferClear(RingBuffer_t* rb);

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */

