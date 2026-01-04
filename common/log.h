/**
 * @file log.h
 * @brief 日志输出宏定义
 * 
 * 提供带文件名和行号的日志输出功能
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>

// ANSI 颜色码
#define ANSI_COLOR_RED     "\033[31m"
#define ANSI_COLOR_RESET   "\033[0m"

/**
 * @brief 带文件名和行号的日志输出宏
 * 
 * 使用方式：
 *   LOG("message");
 *   LOG("value: %d", value);
 */
#define LOG(fmt, ...) \
    printf("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

/**
 * @brief 带文件名和行号的错误日志输出宏（输出到stderr，红色显示）
 */
#define LOG_ERR(fmt, ...) \
    fprintf(stderr, ANSI_COLOR_RED "[%s:%d] ERROR: " fmt ANSI_COLOR_RESET, __FILE__, __LINE__, ##__VA_ARGS__)

#endif /* LOG_H */

