# UExplorer Agent Guide

## 目标与适用范围

UExplorer 是 Windows x64 上的 Unreal Engine SDK Dump 与运行时探索工具。代理的目标是在不破坏目标游戏进程、不掩盖失败、不覆盖用户改动的前提下，交付可复现、可验证的修改。

本文件是仓库级工作约束；`DESIGN.md` 和 `PROJECT_MAP.md` 是设计及历史记录，不保证与当前实现完全同步。

## 不可违反的规则

1. 默认使用简体中文；代码标识符、命令、日志和错误信息保持原文。
2. 先检查 `git status --short --branch`，再读取相关代码并确认真实运行链路。不能验证的内容必须标为推断。
3. 只修改用户授权范围。不得恢复、覆盖、格式化或提交无关改动。
4. 禁止执行删除、清空、重建目录或等价操作。需要移除内容时保留原件并先说明方案。
5. Windows 环境使用 PowerShell；文本搜索优先 `rg`，手工修改一律使用 `apply_patch`。
6. API 验证默认只使用 `curl.exe`；只有用户明确要求时才使用浏览器自动化。
7. **禁止 fallback。** 能力不可用时必须返回明确失败及根因，不得静默切换端口、地址、算法、占位实现、默认凭据、空回调或启发式猜测。若产品确实需要多策略，必须由用户显式选择并在响应中暴露实际策略。
8. 不得把“代码存在”“编译通过”或“接口返回 200”写成“功能验证通过”。涉及注入、目标进程、UE 版本、Dump 文件或 Hook 的结论必须有真实运行证据。
9. 源码修改后必须说明：修改内容、动机、解决的问题、根因、验证结果及未执行项。

## 事实优先级

发生冲突时按以下顺序判断：

1. 目标进程中的实时行为和崩溃/调用栈
2. `curl.exe` 请求响应、SSE/WS 数据和运行日志
3. 当前构建产物及实际加载模块
4. `%LOCALAPPDATA%\UExplorer\connection.ini` 与 `runtime.ini`
5. 当前源代码和项目配置
6. `DESIGN.md`、`PROJECT_MAP.md`、注释和历史提交

不要用设计文档覆盖运行事实。确认文档过期后，在任务授权范围内更新相应状态，而不是继续复制旧描述。

## 当前架构

```text
React 19 + TypeScript + Vite
        |
        | Tauri invoke / HTTP / SSE / WebSocket
        v
Tauri 2 Rust Host
  - 扫描候选进程
  - CreateRemoteThread + LoadLibraryW 注入
  - 读写连接配置和运行时端点
        |
        | 注入 UExplorerCore.dll
        v
目标 UE 游戏进程
  - Main.cpp: 初始化、服务启动、F6 卸载
  - Engine/: GObjects、FName、反射、偏移发现、蓝图字节码
  - Generator/: C++ SDK、USMAP、Dumpspace、IDA 映射
  - API/: 12 个 API 模块、当前注册 65 个 HTTP 路由
  - Server/: WinSock2 HTTP、SSE、WebSocket
  - HookApi + GameThreadQueue: ProcessEvent 监控与 PostRender 调度
```

### 主要文件

- `Dumper/Main.cpp`：DLL 生命周期、连接配置、引擎初始化、HTTP 服务启动。
- `Dumper/Engine/`：从 Dumper-7 派生的 UE 内存模型与偏移发现。
- `Dumper/Generator/`：四类 Dump 生成器及其共享管理器。
- `Dumper/API/Router.cpp`：路由注册与 Hook 初始化入口。
- `Dumper/API/GameThreadQueue.h`：HTTP Worker 到游戏线程的同步调用槽。
- `Dumper/API/HookApi.cpp`：ProcessEvent VTable 监控、PostRender VTable 调度。
- `Dumper/Server/HttpServer.cpp`：自研 WinSock2 协议层和连接生命周期。
- `frontend/src/api/index.ts`：前端类型、连接恢复、REST/SSE/WS 客户端；当前体积较大。
- `frontend/src/pages/`：Dashboard、Objects、Functions、Memory、SDKDump、Settings 六个主页面。
- `frontend/src-tauri/src/lib.rs`：Tauri 命令、进程扫描、DLL 注入和端点配置。

## 运行与连接约定

- Core 只监听 `127.0.0.1`。默认首选端口为 `27015`。
- 除 `/api/v1/status/health` 外，HTTP/SSE 请求使用 `X-UExplorer-Token`。
- WebSocket 当前通过 `?token=` 传递 Token。
- `connection.ini` 保存下一次注入使用的端口和 Token；`runtime.ini` 发布 DLL 实际 PID、端口、Token 和运行状态。
- `uexplorer-dev` 只是前后端启动默认值；Core 首次加载时会将其替换为随机 Token。验证时应读取当前 `runtime.ini`，不得假定固定 Token。
- 当前 `runtime.ini` 是单实例文件；在支持多目标进程之前，不得宣称支持并行连接多个游戏。

## 线程与生命周期硬约束

1. HTTP Handler、Dump Worker、SSE/WS 连接线程都不是 UE 游戏线程。
2. 会触发 UE 行为或依赖 UE 线程亲和性的操作必须经过明确的游戏线程调度；不得因调度器不可用而直接在 HTTP 线程调用 `ProcessEvent`。
3. 对象索引不是稳定身份。跨请求保存对象时必须重新校验索引、地址及可用的 serial/generation，防止 GC 后索引复用。
4. Hook 热路径不得执行 socket I/O、JSON 序列化、无界分配或长时间持锁。热路径只允许写入有界队列，由非游戏线程发送事件。
5. DLL 卸载前必须先停止接收新工作、唤醒等待者、关闭所有连接、等待 Worker 退出、恢复 Hook，最后才能 `FreeLibraryAndExitThread`。不得依赖 detached thread 自行结束。
6. Dump 生成器共享大量全局状态。在完成上下文隔离前，必须显式限制为单任务，不得伪装成可并行执行。
7. 所有超时都要定义所有权：调用超时后，目标线程不得继续访问调用方栈或临时缓冲区。

## 已确认的技术债基线（2026-08-09）

以下项目不能按“已完成”处理，修改相关模块时应优先建立最小复现：

- `HttpServer` 为每个客户端创建 detached thread；`Stop()` 最多等待约 3 秒，仍存在卸载期悬挂线程和 `Impl` 生命周期风险。
- `GameThreadQueue` 只有一个同步槽；调用超时与参数缓冲区生命周期仍可能竞争。
- Hook 命中路径会同步构造 JSON 并广播到 SSE/WS，慢客户端可能阻塞游戏线程。
- `WatchApi` 没有独立轮询线程；`interval_ms` 未用于调度，变化检测实际由 `GET /watch/list` 驱动。
- `/ws/console` 目前只返回连接占位消息，不是实际 UE Console 桥。
- SDK Dump 页面会提交筛选/生成选项，但 `DumpApi` 当前未解析请求体，选项不生效。
- `DumpApi` 用 `joinable()` 判断“已完成”线程；启动后续任务时可能同步等待前一个任务。
- `MappingGenerator` 声明 ZStandard 压缩但写入未压缩 payload，当前 USMAP 产物格式存在不一致风险。
- Tauri 注入器仅等待远程线程结束，未校验 `LoadLibraryW` 返回值；超时后释放远程参数内存存在目标线程仍在使用的风险，也未验证目标架构与 DLL 架构一致。
- 运行时端点文件可能在异常退出后残留；采用端点前需要验证 PID、进程存活和 health，而不是只相信 `Running=1`。
- 仓库当前没有自动化测试或 CI。前端 `npm run build` 可通过，但 `npm run lint` 基线为 15 errors / 16 warnings；不得表述为“前端检查全绿”。

## 修改流程

1. 确认工作树和用户现有改动，记录本任务允许修改的文件。
2. 从入口沿真实调用链定位问题；优先复现一个最窄的端到端路径。
3. 先写清楚失败条件和预期行为，再做小而可审查的修改。
4. 只运行与改动直接相关的最小充分验证。涉及目标游戏的行为若当前无法运行，明确写“未做目标进程验证”。
5. 源码修改后检查 `DESIGN.md`；只有项目进度或架构事实改变时才更新，禁止为了勾选进度而改文档。
6. 验证通过后只暂存本任务文件，提交并推送当前分支；不得把用户的无关删除或未跟踪文件带入提交。文档/审查任务默认不自动提交或推送，除非用户明确要求。

## 验证命令

### Core DLL（修改 C++ 后必须运行）

```powershell
powershell -Command "& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' 'D:\Projects\UExplorer\Dumper\UExplorerCore.vcxproj' /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal 2>&1"
```

输出：`D:\Projects\UExplorer\Dumper\x64\Release\UExplorerCore.dll`

### React/Vite（修改前端后按相关性运行）

```powershell
Set-Location D:\Projects\UExplorer\frontend
npm run build
npm run lint
```

### Tauri/Rust（修改 `src-tauri` 后必须运行）

```powershell
cargo check --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml
```

涉及打包配置、图标、权限或安装器时，再运行：

```powershell
Set-Location D:\Projects\UExplorer\frontend
npm run tauri:build
```

### API（DLL 已注入且 runtime.ini 有效时）

先读取当前运行端点，再使用 `curl.exe`；不要复制旧 Token：

```powershell
Get-Content -LiteralPath "$env:LOCALAPPDATA\UExplorer\runtime.ini"
curl.exe -H "X-UExplorer-Token: <runtime-token>" "http://127.0.0.1:<runtime-port>/api/v1/status"
```

## 交付说明

最终答复按“结果 -> 关键改动 -> 验证 -> 未完成/风险”组织。不得隐瞒失败，不得把 fallback 描述成兼容性，不得声称未运行的检查已经通过。
