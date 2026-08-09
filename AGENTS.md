# UExplorer Agent Guide

## 1. 项目与目标

UExplorer 是 Windows x64 上的 Unreal Engine SDK 生成与运行时内省桌面工具。系统跨越两个信任与生命周期边界：

```text
React 19 + TypeScript + Vite
  -> Tauri invoke / tauri::ipc::Channel
Rust Host
  -> versioned Windows Named Pipe RPC
UExplorerCore.dll（目标进程内）
  -> 经验证的 UE 内存、反射与游戏线程能力
```

本仓库正在按 `REFACTOR_PLAN.md` 执行 R0-R7 重构。问题是否关闭只以 `docs/issue-status.json` 的可复现证据为准；代码存在、编译成功或 UI 存在都不等于功能可用。

## 2. 不可违反的规则

1. 默认使用简体中文；代码标识符、命令、日志和错误保持原文。
2. 开始工作先检查 `git status --short --branch`，再沿当前运行入口确认事实。无法验证的内容必须标为推断。
3. 只修改用户授权范围。不得恢复、覆盖、格式化、暂存或提交无关改动。
4. **禁止删除文件、清空目录、重建目录或执行等价破坏性操作。** 需要退役代码时保留原件，切断编译/运行入口并记录归档原因。
5. Windows 环境使用 PowerShell；文本搜索优先 `rg`；手工修改一律使用 `apply_patch`。
6. **禁止 fallback。** 不得静默切换 transport、endpoint、PID/session、Offset、执行线程、协议版本、压缩算法、凭据、占位数据或替代实现。能力不可用必须返回稳定错误码和根因。
7. 不得把“静态检查通过”“能编译”“接口返回成功”写成目标 UE 行为已验证。涉及注入、GC、Hook、Dump、UE profile 或卸载的结论必须有对应目标进程 fixture。
8. 每个源码修改检查点必须说明：修改内容、动机、解决的问题、根因、验证结果及未执行项。
9. 每个源码修改检查点必须更新 `DESIGN.md` 的当前进度，明确暂存本次文件，提交并推送当前分支；**推送成功后**运行第 9.1 节的精确 Core Release 构建命令。
10. 当前桌面主链路没有 HTTP API。若未来显式实现 Host Gateway，API 手工验证默认只用 `curl.exe`；除非用户明确要求，不使用浏览器自动化。

## 3. 当前运行事实（R4 完成后）

- React 领域调用只进入 `frontend/src/api/client.ts`，并调用 Tauri `domain_request`；事件只通过调用方持有的 Tauri Channel。
- Rust `DomainService` 使用显式 operation 白名单，绑定明确 target PID 或 active session。未知 operation 在接触 session 前返回 `OPERATION_NOT_SUPPORTED`。
- 当前 Host 已实现 status 和 immutable snapshot-backed Object/Type 基础查询。Core 会从一代稳定 Object Snapshot 自动尝试 witnessed ReflectionLayout 采集；只有完整 U/FProperty 证据成功才开放 `engine.reflection`。通用 `TypeSnapshotCapture` owner 已实现，但 PropertyCodec/type metadata 生产源、type Main 调度接线以及 Property、Memory、Call、World、Watch、Hook、Blueprint、Dump 等领域仍不可用；不得伪造字段或回退旧实现。
- Core release project 只运行 PID-scoped Named Pipe；不编译 `Dumper/Server/HttpServer.cpp` 和 `Dumper/API/*.cpp`，不链接 `ws2_32`。旧 HTTP/SSE/WebSocket/API 源码保留为历史证据，不是兼容层。
- Core 在 Pipe bind 后安装生产 `PostRenderHook`，发布事实 capability/Ready 后才开放 admissions。
- 当前没有外部 HTTP/WebSocket Gateway，也没有 `connection.ini`、`runtime.ini`、port 或 Token 运行依赖。
- 未有任何 UE 4.26、4.27 或 UE5 profile 达到发布支持门；准确范围见 `docs/SUPPORT_MATRIX.md`。
- 已知仍未关闭的事实包括：默认 `AllocConsole` 与 F6 路径、Dumper 配置的 current-directory/global-path 行为、剩余 `Off::*/Settings::*` 和 legacy domain monolith、未实现领域命令、Hook/Watch producer、前端 session/query/BigInt 状态重构以及真实 UE/GC/卸载/性能 fixture。

## 4. 事实优先级

证据冲突时按以下顺序判断：

1. 目标进程实时行为、崩溃/调用栈和可重复 trace。
2. 实际 Named Pipe frame、Host session/event 诊断和注入阶段结果。
3. 当前加载的 DLL、release project 输入、import table 与二进制 marker。
4. 当前进程配置和已发布的 immutable EngineContext/capability/snapshot。
5. 当前源码与测试。
6. `DESIGN.md`、`PROJECT_MAP.md`、注释、legacy 源码和历史提交。

使用源码解释运行行为，不得用历史文档覆盖当前二进制事实。

## 5. 关键代码边界

### Core DLL

- `Dumper/Main.cpp`：唯一 DLL 生命周期装配入口；负责 Runtime、Facade、Pipe、PostRender 和有序关闭。
- `Dumper/Runtime/`：CoreRuntime、EngineContext、Capability、SafeMemory、Handle、Snapshot、GameThread、Hook owner；`ObjectSnapshotReflectionCandidateSource.*` 是唯一生产 reflection candidate 边界，不得重新接入 `Off::InitReflection()` 作为 fallback；`TypeSnapshotCapture.*` 只是通用分帧/Worker publication owner，不得把它描述为生产 type metadata source。
- `Dumper/Services/CoreCommandService.*`：transport-neutral Core command 边界。
- `Dumper/IPC/NamedPipeRpcServer.*`：唯一 release transport。
- `Dumper/Engine/` 与 `Dumper/Generator/`：Dumper-7 派生的 UE 模型和生成器；仍含待迁移的版本/布局假设。
- `Dumper/API/` 与 `Dumper/Server/`：非 release legacy archive。不得从 Main、Host 或 React 恢复可达性。

### Rust Host

- `frontend/src-tauri/src/services/domain_service.rs`：桌面领域 operation registry、schema/capability/session gate。
- `frontend/src-tauri/src/session/`：多 PID SessionManager、SnapshotCache、EventHub、Tauri event bridge。
- `frontend/src-tauri/src/ipc/`：严格 RPC session 和 overlapped Named Pipe client。
- `frontend/src-tauri/src/lib.rs`：Tauri commands、进程扫描、注入和 managed state 装配。
- `frontend/src-tauri/src/inject_dll.ps1`：已禁用历史脚本，不是 fallback。

### React

- `frontend/src/api/index.ts`：共享 API/domain 类型和 client export。
- `frontend/src/api/client.ts`：唯一 Tauri transport adapter。
- `frontend/src/pages/`：页面；只能消费 typed client，不得直接创建网络连接或读取运行 endpoint。
- `frontend/src/types/`：页面模型；x64 地址必须保持规范 hex string 或 BigInt，不得转成 JS `Number` 计算。

## 6. 线程、身份与生命周期硬约束

1. Pipe worker、Host worker 和前端线程都不是 UE 游戏线程。依赖 UE 线程亲和性的命令必须进入已验证的 `GameThreadExecutor`。
2. 同步请求不得从已观测 pump thread 等待自身；队列满、deadline、取消和 shutdown 都必须产生唯一终态。
3. 跨请求对象身份必须使用 session/context generation/serial/address/class fingerprint 组成的 Handle；裸 index 或短名只能用于搜索，不得用于危险执行。
4. Hook 热路径只允许有界、非阻塞、低分配采集；不得执行 JSON、Pipe I/O、长时间持锁或等待 Host。
5. 关闭顺序必须先拒绝新工作并完成 Pipe pending，再恢复 Hook、排空 snapshot/facade/request lease，最后才可 `FreeLibraryAndExitThread`。任一 restore/drain 失败都必须拒绝卸载。
6. Dump/Generator 共享大量全局状态；在隔离完成前一次只允许一个 owned job，不能伪装并发。
7. 每个 timeout 都必须定义 buffer、ticket、OVERLAPPED、远程参数和 worker 的所有权；调用方返回后后台不得访问调用方栈或临时缓冲。
8. 配置、协议、capability 和 snapshot 都按 session/generation 绑定；不得相信陈旧磁盘运行态或跨 PID 复用状态。

## 7. 修改流程

1. 记录工作树现状和本次允许修改的文件。
2. 从真实入口证明一条最窄端到端路径，再扩展修改范围。
3. 先定义稳定成功/失败条件和所有权，再做小而可审查的变更。
4. 同步增加最小回归测试或 contract；对故障路径至少验证一个确定性失败。
5. 运行与改动直接相关的最小充分检查；修改跨层契约时运行完整相关矩阵。
6. 更新 `DESIGN.md`；架构/文件职责变化时同步更新 `README.md`、`PROJECT_MAP.md`、`AGENTS.md` 和 issue register。
7. 用显式路径暂存本次文件，检查 staged diff，提交并推送；不得使用 `git add -A` 或 `git add .`。
8. 推送后执行精确 Core Release 构建并记录结果。失败就明确报告，不能宣称检查点完成。

## 8. Issue 与阶段口径

- `REFACTOR_PLAN.md`：128 项问题、R0-R7 顺序和 Definition of Done。
- `docs/issue-status.json`：机器可读状态；只有带复现命令/fixture 的问题可标 `verified`。
- `DESIGN.md` 第 0 节：当前事实和阶段证据。
- `PROJECT_MAP.md`：当前文件/调用边界；标为 legacy 的章节只用于历史审计。
- `docs/SUPPORT_MATRIX.md`：UE profile 支持声明；没有 target fixture 就保持 `Not supported`。
- `protocol/v1/`：IPC v1 唯一契约源。major 不兼容立即失败；minor 能力只经显式协商开放。

## 9. 验证命令

### 9.1 Core Release（每个源码检查点推送后必须原样运行）

```powershell
powershell -Command "& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' 'D:\Projects\UExplorer\Dumper\UExplorerCore.vcxproj' /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal 2>&1"
```

输出：`D:\Projects\UExplorer\Dumper\x64\Release\UExplorerCore.dll`。

### 9.2 Frontend

```powershell
Set-Location D:\Projects\UExplorer\frontend
npm run lint
npm run test
npm run build
```

### 9.3 Rust Host

```powershell
cargo fmt --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml --all -- --check
cargo clippy --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml --all-targets --all-features -- -D warnings
cargo test --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml
```

修改跨语言 transport/session/injection 时还要运行：

```powershell
cargo test --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml --features cross-language-fixture --test core_process_fixture
cargo test --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml --features cross-language-fixture --test injection_process_fixture
```

### 9.4 Core harness 与 contracts

```powershell
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' 'D:\Projects\UExplorer\tests\core-harness\CoreHarness.vcxproj' /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m:1 /v:minimal
& 'D:\Projects\UExplorer\tests\core-harness\x64\Release\CoreHarness.exe' 'D:\Projects\UExplorer\protocol\v1\fixtures'
& 'D:\Projects\UExplorer\tests\contracts\verify-transport-cutover.ps1' -DllPath 'D:\Projects\UExplorer\Dumper\x64\Release\UExplorerCore.dll'
```

其余 contract 按 `README.md` 列表运行；阶段检查点应运行全部 contract，不能只跑新增脚本。

## 10. 交付格式

按“结果 -> 关键改动 -> 根因/解决的问题 -> 验证 -> 未完成/风险”报告。只摘录决定性输出；不得隐藏失败、把未运行项写成通过，或把 capability unavailable 描述为兼容性。
