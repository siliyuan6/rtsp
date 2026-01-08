/**
 * @file log.c
 * @brief 日志输出实现
 */

#include "log.h"

// 调试输出控制标志（默认0，不输出DEBUG）
int g_log_debug_enabled = 0;

/**
 * @brief 启用或禁用DEBUG日志输出
 */
void SetLogDebugEnabled(int enabled)
{
    g_log_debug_enabled = enabled ? 1 : 0;
}

