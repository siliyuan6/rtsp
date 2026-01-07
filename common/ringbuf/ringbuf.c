/**
 * @file ringbuf.c
 * @brief 循环队列（环形缓冲区）模块实现
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include "ringbuf.h"
#include "log.h"

/**
 * @brief 创建循环队列
 */
RingBuffer_t* RingBufferCreate(size_t capacity)
{
    if (capacity == 0)
    {
        LOG_ERR("Invalid capacity: 0\n");
        return NULL;
    }

    RingBuffer_t* rb = (RingBuffer_t*)malloc(sizeof(RingBuffer_t));
    if (NULL == rb)
    {
        LOG_ERR("Failed to allocate RingBuffer_t\n");
        return NULL;
    }

    memset(rb, 0, sizeof(RingBuffer_t));

    rb->buffer = (unsigned char*)malloc(capacity);
    if (NULL == rb->buffer)
    {
        LOG_ERR("Failed to allocate buffer\n");
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->readPos = 0;
    rb->writePos = 0;
    rb->dataSize = 0;
    rb->isClosed = 0;

    if (pthread_mutex_init(&rb->mutex, NULL) != 0)
    {
        LOG_ERR("Failed to initialize mutex\n");
        free(rb->buffer);
        free(rb);
        return NULL;
    }

    if (pthread_cond_init(&rb->condRead, NULL) != 0)
    {
        LOG_ERR("Failed to initialize condRead\n");
        pthread_mutex_destroy(&rb->mutex);
        free(rb->buffer);
        free(rb);
        return NULL;
    }

    if (pthread_cond_init(&rb->condWrite, NULL) != 0)
    {
        LOG_ERR("Failed to initialize condWrite\n");
        pthread_cond_destroy(&rb->condRead);
        pthread_mutex_destroy(&rb->mutex);
        free(rb->buffer);
        free(rb);
        return NULL;
    }

    return rb;
}

/**
 * @brief 销毁循环队列
 */
void RingBufferDestroy(RingBuffer_t* rb)
{
    if (NULL == rb)
    {
        return;
    }

    pthread_mutex_lock(&rb->mutex);
    rb->isClosed = 1;
    pthread_cond_broadcast(&rb->condRead);
    pthread_cond_broadcast(&rb->condWrite);
    pthread_mutex_unlock(&rb->mutex);

    pthread_cond_destroy(&rb->condRead);
    pthread_cond_destroy(&rb->condWrite);
    pthread_mutex_destroy(&rb->mutex);

    if (rb->buffer != NULL)
    {
        free(rb->buffer);
    }

    free(rb);
}

/**
 * @brief 写入数据到循环队列
 */
int RingBufferPush(RingBuffer_t* rb, const void* data, size_t size)
{
    if (NULL == rb || NULL == data || size == 0)
    {
        return -1;
    }

    pthread_mutex_lock(&rb->mutex);

    // 检查是否已关闭
    if (rb->isClosed)
    {
        pthread_mutex_unlock(&rb->mutex);
        return -1;
    }

    // 等待有足够空间
    while ((rb->capacity - rb->dataSize) < size)
    {
        if (rb->isClosed)
        {
            pthread_mutex_unlock(&rb->mutex);
            return -1;
        }
        pthread_cond_wait(&rb->condWrite, &rb->mutex);
    }

    // 写入数据（可能需要分两次写入，如果跨越缓冲区边界）
    size_t remaining = size;
    const unsigned char* src = (const unsigned char*)data;

    while (remaining > 0)
    {
        // 计算从writePos到缓冲区末尾的可用空间
        size_t toEnd = rb->capacity - rb->writePos;
        size_t toWrite = (remaining < toEnd) ? remaining : toEnd;

        memcpy(rb->buffer + rb->writePos, src, toWrite);

        rb->writePos = (rb->writePos + toWrite) % rb->capacity;
        rb->dataSize += toWrite;
        remaining -= toWrite;
        src += toWrite;
    }

    // 唤醒等待读取的线程
    pthread_cond_broadcast(&rb->condRead);
    pthread_mutex_unlock(&rb->mutex);

    return 0;
}

/**
 * @brief 从循环队列读取数据
 */
int RingBufferPop(RingBuffer_t* rb, void* data, size_t maxSize, 
                  size_t* actualSize, int timeoutMs)
{
    if (NULL == rb || NULL == data || NULL == actualSize || maxSize == 0)
    {
        return -1;
    }

    *actualSize = 0;

    pthread_mutex_lock(&rb->mutex);

    // 计算超时时间
    struct timespec timeout;
    int useTimeout = (timeoutMs >= 0);
    if (useTimeout)
    {
        struct timeval now;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec + timeoutMs / 1000;
        timeout.tv_nsec = (now.tv_usec + (timeoutMs % 1000) * 1000) * 1000;
        if (timeout.tv_nsec >= 1000000000)
        {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
    }

    // 等待有数据可读
    while (rb->dataSize == 0)
    {
        if (rb->isClosed)
        {
            pthread_mutex_unlock(&rb->mutex);
            return -1;
        }

        if (timeoutMs == 0)
        {
            // 非阻塞模式
            pthread_mutex_unlock(&rb->mutex);
            return 1; // 超时
        }

        int ret;
        if (useTimeout)
        {
            ret = pthread_cond_timedwait(&rb->condRead, &rb->mutex, &timeout);
        }
        else
        {
            ret = pthread_cond_wait(&rb->condRead, &rb->mutex);
        }

        if (ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&rb->mutex);
            return 1; // 超时
        }
    }

    // 读取数据
    size_t toRead = (maxSize < rb->dataSize) ? maxSize : rb->dataSize;
    size_t remaining = toRead;
    unsigned char* dst = (unsigned char*)data;

    while (remaining > 0)
    {
        // 计算从readPos到缓冲区末尾的可用数据
        size_t toEnd = rb->capacity - rb->readPos;
        size_t toReadNow = (remaining < toEnd) ? remaining : toEnd;

        memcpy(dst, rb->buffer + rb->readPos, toReadNow);

        rb->readPos = (rb->readPos + toReadNow) % rb->capacity;
        rb->dataSize -= toReadNow;
        remaining -= toReadNow;
        dst += toReadNow;
    }

    *actualSize = toRead;

    // 唤醒等待写入的线程
    pthread_cond_broadcast(&rb->condWrite);
    pthread_mutex_unlock(&rb->mutex);

    return 0;
}

/**
 * @brief 获取当前数据大小
 */
size_t RingBufferGetSize(RingBuffer_t* rb)
{
    if (NULL == rb)
    {
        return 0;
    }

    pthread_mutex_lock(&rb->mutex);
    size_t size = rb->dataSize;
    pthread_mutex_unlock(&rb->mutex);

    return size;
}

/**
 * @brief 获取空闲空间大小
 */
size_t RingBufferGetFree(RingBuffer_t* rb)
{
    if (NULL == rb)
    {
        return 0;
    }

    pthread_mutex_lock(&rb->mutex);
    size_t free = rb->capacity - rb->dataSize;
    pthread_mutex_unlock(&rb->mutex);

    return free;
}

/**
 * @brief 关闭缓冲区
 */
void RingBufferClose(RingBuffer_t* rb)
{
    if (NULL == rb)
    {
        return;
    }

    pthread_mutex_lock(&rb->mutex);
    rb->isClosed = 1;
    pthread_cond_broadcast(&rb->condRead);
    pthread_cond_broadcast(&rb->condWrite);
    pthread_mutex_unlock(&rb->mutex);
}

/**
 * @brief 清空缓冲区
 */
void RingBufferClear(RingBuffer_t* rb)
{
    if (NULL == rb)
    {
        return;
    }

    pthread_mutex_lock(&rb->mutex);
    rb->readPos = 0;
    rb->writePos = 0;
    rb->dataSize = 0;
    pthread_cond_broadcast(&rb->condWrite);
    pthread_mutex_unlock(&rb->mutex);
}

