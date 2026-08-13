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

## 3. 当前运行事实（R5 代码检查点）

- React 领域调用只进入 `frontend/src/api/client.ts`，并调用 Tauri `domain_request`；事件只通过调用方持有的 Tauri Channel。
- Rust `DomainService` 使用显式 operation 白名单，绑定明确 target PID 或 active session。未知 operation 在接触 session 前返回 `OPERATION_NOT_SUPPORTED`。
- 当前 Host 已实现 status、immutable snapshot-backed Object/Type 集合查询、exact-path Class/Struct/Enum/Function 详情、exact stable-handle `objects.property.read`、R5.2 单目标 `call.invoke`、R5.3 World 查询与 `world.actor.transform.get/update`，以及严格的 Memory、Watch、Hook、Dump 和 Blueprint operation 转发。Core 会从一代稳定 Object Snapshot 采集 witnessed ReflectionLayout 和完整 TypeSnapshot，canonical FVector/FRotator 只接受 exact CoreUObject path、语义字段、float/double kind、offset、size 与 alignment，不按版本或 LWC 猜测。World transform read 在一个 owned game-thread work 中固定 exact generation，显式区分 RootComponent stored 值与 reflected getter computed 值；update 只开放单字段 exact reflected setter：world/relative scale 与 world rotation。location 和 relative rotation 因 `FHitResult` 生命周期未见证而稳定拒绝，不做 raw write 或多字段伪事务。
- Memory 已有严格 canonical address/typed codec、最大 4096-byte raw access、64-step pointer chain、写前 preimage、写后验证、受限 rollback、保护状态竞态检测和 executable-page 拒绝。Watch 已有 session/generation-bound 有界 scheduler、逐帧预算、按条数与字节限制的 history、drop/coalesce 诊断和显式 `watch.events.drain` 拉取；每个订阅以 owned binding 固定 exact immutable Object/Type/Reflection snapshots，周期刷新无需冻结全局快照，disabled watch 也不静默 rebind。它当前不是 Named Pipe Event/Tauri Channel push producer。二者只有代码与合成边界证据，没有目标 UE fixture。
- `call.invoke` 现支持 descriptor-proven enum name/raw 输入和输出解码，`ParamFrame` 拥有逆序 destructor journal；但生产类型源没有 witnessed `UEnum::Names` entry-table layout，所以真实目标枚举 descriptor 仍保持 unavailable。`call.batch` 已由 Main/Core/Host/schema/TypeScript 装配：单 active coordinator 拥有最多 64 项、总 deadline、取消和 retained result；每项只复用 `FunctionCallCommandService::PrepareInvoke -> GameThreadExecutor -> CompleteInvoke`，snapshot 漂移稳定失败，不做 direct ProcessEvent/fallback。FString/FText/Weak/Soft、任意 Struct/Array/Map/Set 的输入生命周期仍未开放。
- Blueprint raw bytecode capture 已由 Main 接入 generation-bound runtime source：只有 `UFunction::Script` 通过 high-confidence multi-function witness、当前 Object/Type snapshot 完全匹配且 game-thread executor 可用时，`blueprint.bytecode` 才动态发布；捕获前后复核 exact FunctionHandle/TArray header/依赖，且 payload 必须以 `EX_EndOfScript` 结束。UE4/UE5 opcode 值存在差异，当前没有 exact opcode/operand profile，因此 `blueprint.decompile` 继续 false，不按版本字符串猜测。Hook 已有预分配有界 collector 和 exact FunctionHandle command/state 原语，但没有真实 ProcessEvent producer/patch；Dump 已有单 active-job coordinator 和严格 command 原语，但没有 generator worker。Hook、Dump 不得因源码存在被描述为运行时可用；call.batch 与 raw Blueprint capture 虽已代码可达，也不得在真实 UE fixture 前描述为已验证。
- computed transform、Memory、Watch、enum、Blueprint、Hook、Dump 与 batch 当前都没有 Wandering Sword/真实 UE 行为证据；Wandering Sword 目录没有可启动游戏 `.exe`，所有 profile 仍为 `Not supported`。
- Core release project 只运行 PID-scoped Named Pipe；不编译 `Dumper/Server/HttpServer.cpp` 和 `Dumper/API/*.cpp`，不链接 `ws2_32`。旧 HTTP/SSE/WebSocket/API 源码保留为历史证据，不是兼容层。
- Core 在 Pipe bind 后安装生产 `PostRenderHook`，发布事实 capability/Ready 后才开放 admissions。
- 当前没有外部 HTTP/WebSocket Gateway，也没有 `connection.ini`、`runtime.ini`、port 或 Token 运行依赖。
- 未有任何 UE 4.26、4.27 或 UE5 profile 达到发布支持门；本机 UE 4.21、4.24-4.27、5.0-5.4、5.6、5.7 源码只能作为候选布局/语义证据，Wandering Sword 也必须通过实际运行 fixture 后才能改变支持声明。准确范围见 `docs/SUPPORT_MATRIX.md`。
- 已知仍未关闭的事实包括：默认 `AllocConsole` 与 F6 路径、Dumper 配置的 current-directory/global-path 行为、剩余 `Off::*/Settings::*` 和 legacy domain monolith、Hook ProcessEvent producer、Watch push producer、Dump generator worker/output-root/snapshot pin、生产 UEnum entry table 与 Blueprint opcode/operand profile witness、call.batch/Blueprint 真实 ProcessEvent/捕获/取消/超时 fixture、前端 session/query 状态重构以及真实 UE/GC/卸载/性能 fixture。

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
- `Dumper/Runtime/`：CoreRuntime、EngineContext、Capability、SafeMemory、Handle、Object/Type/World Snapshot、GameThread 与 Hook owner；`WatchScheduler.*`、`HookEventCollector.*`、`DumpJobCoordinator.*` 和 `FunctionCallBatchCoordinator.*` 分别拥有 bounded watch、hook event、single dump job 和 call batch 原语；`BlueprintBytecodeCapture.*`/`BlueprintBytecodeEvidence.*` 只接受显式 Script layout/profile witness，`BlueprintBytecodeRuntime.*` 按 exact snapshot generation 管理生产 capture store/source 与 drain。`ObjectSnapshotReflectionCandidateSource.*` 是唯一生产 reflection candidate 边界，`ObjectSnapshotTypeCandidateSource.*` 是 exact snapshot/layout 上的生产结构类型源，`TypeSnapshotCapture.*` 负责通用分帧、依赖封口与 Worker publication，`WorldSnapshotCapture.*` 负责 World 关系的分帧采集和复核。不得重新接入 `Off::InitReflection()` 或 legacy wrapper 作为 fallback。
- `Dumper/Services/CoreCommandService.*`：transport-neutral Core command 总入口、lease/capability/snapshot gate。
- `Dumper/Services/TypeCommandService.*`：worker-only immutable TypeSnapshot 详情服务；执行身份必须是 exact full path，成员 scope 必须显式，分页必须使用 session/context/generation/query-bound cursor。
- `Dumper/Services/FunctionCallCommandService.*`、`FunctionCallBatchCommandService.*`、`Dumper/Runtime/FunctionCallBatchCoordinator.*` 与 `ParamFrame.*`：单目标 exact ProcessEvent 及复用该路径的单 active bounded batch、精确参数 schema、owned frame、canonical FVector/FRotator、descriptor-proven enum codec、总 deadline/取消/逐项结果和逆序 destructor journal；生产 UEnum table 与任意 Struct/复杂生命周期类型尚未开放。
- `Dumper/Services/MemoryCommandService.*`：strict raw/typed/pointer-chain 命令；只允许 data write，不提供 executable/code-write fallback。
- `Dumper/Services/WatchCommandService.*`：exact Handle/generation watch CRUD、snapshot 与显式 pull drain；不是 Tauri Channel push producer。
- `Dumper/Services/BlueprintCommandService.*`：exact FunctionHandle/generation bytecode capture/decompile gate；缺 capture/profile source 时稳定 unavailable。
- `Dumper/Services/HookCommandService.*` 与 `Dumper/Services/DumpCommandService.*`：已实现的 transport-neutral command 原语；因缺真实 ProcessEvent producer 或 generator worker，当前 capability 仍为 false。
- `Dumper/Services/WorldCommandService.*`：worker-only immutable WorldSnapshot 查询；开放 inspect、Level/Actor/Component page、exact-handle Actor detail 与显式状态 shortcuts。所有 cursor 绑定 generation/query，Component cursor 额外绑定 ActorHandle。
- `Dumper/Services/WorldTransformCommandService.*`：game-thread same-witness RootComponent stored transform 读取，并在同一 owned work 内以 exact reflected Actor getter + owned return frame取得 computed world location/rotation/scale；两类值显式分离，computed 能力不可用时返回稳定原因。
- `Dumper/Services/WorldMutationCommandService.*`：单字段、generation-bound、exact-handle reflected setter。当前仅允许 world/relative scale 与 world rotation；location/relative rotation fail closed，调用后身份复核失败明确报告 mutation state unknown。
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
