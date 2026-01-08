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
#define ANSI_COLOR_YELLOW  "\033[33m"
#define ANSI_COLOR_GREEN   "\033[32m"
#define ANSI_COLOR_WHITE   "\033[37m"
#define ANSI_COLOR_RESET   "\033[0m"

// 调试输出控制标志（默认0，不输出DEBUG）
extern int g_log_debug_enabled;

/**
 * @brief 启用或禁用DEBUG日志输出
 * 
 * @param enabled 1表示启用，0表示禁用
 */
void SetLogDebugEnabled(int enabled);

/**
 * @brief 带文件名和行号的警告日志输出宏（输出到stderr，黄色显示）
 */
#define LOG_WARN(fmt, ...) \
    fprintf(stderr, ANSI_COLOR_YELLOW "[%s:%d] WARN: " fmt ANSI_COLOR_RESET, __FILE__, __LINE__, ##__VA_ARGS__)

/**
 * @brief 带文件名和行号的信息日志输出宏（输出到stdout，绿色显示）
 */
#define LOG_INFO(fmt, ...) \
    printf(ANSI_COLOR_GREEN "[%s:%d] " fmt ANSI_COLOR_RESET, __FILE__, __LINE__, ##__VA_ARGS__)

/**
 * @brief 带文件名和行号的调试日志输出宏（输出到stdout，白色显示）
 * 
 * 只有当g_log_debug_enabled为1时才输出
 */
#define LOG_DEBUG(fmt, ...) \
    do { \
        if (g_log_debug_enabled) { \
            printf(ANSI_COLOR_WHITE "[%s:%d] DEBUG: " fmt ANSI_COLOR_RESET, __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

/**
 * @brief 带文件名和行号的错误日志输出宏（输出到stderr，红色显示）
 */
#define LOG_ERR(fmt, ...) \
    fprintf(stderr, ANSI_COLOR_RED "[%s:%d] ERROR: " fmt ANSI_COLOR_RESET, __FILE__, __LINE__, ##__VA_ARGS__)

#endif /* LOG_H */

