# Repository Guidelines

## Project Structure & Module Organization
- `rtsp_client/` contains the C client implementation (`rtsp.c`, `rtp.c`, headers) and a sample stream file (`rtsp_stream.h264`).
- `rtsp_server/` contains the C server implementation (`rtsp_server.c`).
- `resource/` stores larger media assets (for example `704x576_pal_baseLine.h264`) used for testing or demos.
- Top-level `README.md` is minimal; prefer documenting module-specific behavior in the relevant folder.

## Build, Test, and Development Commands
- `cd rtsp_client; make` builds the client executable (`rtsp.exe`) with Winsock support.
- `cd rtsp_client; make clean` removes generated executables.
- `cd rtsp_server; make` builds the server executable (`rtsp_server.exe`) with warnings enabled.
- `cd rtsp_server; make clean` removes the server executable.

## Coding Style & Naming Conventions
- Language: C targeting Windows (Winsock). Keep changes compatible with `gcc` and the existing Makefiles.
- Indentation appears to be tabs; follow the existing style in each file.
- Functions use `snake_case` (for example `create_rtsp_socket`) and constants use `UPPER_SNAKE_CASE`.
- Keep filenames lowercase with underscores (for example `rtsp_server.c`).

## Testing Guidelines
- No automated tests are present. Validate manually by running `rtsp_server.exe` and connecting with the client or an RTSP-capable player.
- If you add tests, document how to run them in the relevant module folder.

## Commit & Pull Request Guidelines
- Commit messages are short and descriptive, usually in past tense (for example "Add rtsp server code.") with occasional tags like `[feat]` or `[describe]`.
- PRs should include: a brief summary, build/run steps (commands), and expected output or sample logs. Include sample media details if behavior depends on a specific `.h264` asset.

## Configuration & Assets
- Media assets are large; keep them under `resource/` or module-specific folders and reference them by relative path.
- If adding new demo streams, note codec and resolution in the filename.
