# rtsp

一个基于 C 的简易 RTSP 客户端与服务端示例，面向 Windows（Winsock）。仓库包含客户端、服务端以及用于演示的 H.264 码流资源。

## 目录结构
- `rtsp_client/`：客户端实现（`rtsp_client.c`, `rtp.c`, `player.c`, 头文件），以及示例码流 `rtsp_stream.h264`。
- `rtsp_server/`：服务端实现（`rtsp_server.c`）。
- `resource/`：较大的媒体资源文件（例如 `704x576_pal_baseLine.h264`）。

如需使用外部播放器测试，可尝试：
```
rtsp://localhost:8554/
```

## 编码与风格约定
- 语言：C（Windows + Winsock）。
- 缩进：与现有文件保持一致（主要使用制表符）。
- 命名：函数 `snake_case`，常量 `UPPER_SNAKE_CASE`，文件名小写下划线。

## 注意事项
- 目前无自动化测试，请以手动联调为主。
- 大文件资源请放在 `resource/` 或模块目录内，并使用相对路径引用。
- 若新增码流示例，建议在文件名中注明编码和分辨率（如 `h264_704x576.h264`）。
