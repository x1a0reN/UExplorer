# UExplorer — 功能设计与页面架构方案

## 0. 当前事实与重构状态（2026-08-09）

本节是当前状态入口，优先级高于下方历史设计。下方带“已完成”的旧阶段记录只表示曾存在对应代码或界面，不代表功能正确、线程安全或已经过目标进程验证。

完整问题、证据等级、目标架构和 R0-R7 验收门见 `REFACTOR_PLAN.md`；支持范围见 `docs/SUPPORT_MATRIX.md`；进程边界决策见 `docs/adr/0001-process-boundary-and-ipc.md`。

目标运行链路已经确定为：

```text
React -> Tauri invoke/event -> Rust Host -> Windows Named Pipe RPC -> Core DLL -> Unreal Engine
```

- Core DLL 发布构建已移除可达的 HTTP、SSE 和 WebSocket 运行路径；旧源文件只作历史证据保留。
- 当前未提供外部 HTTP/WebSocket Gateway；未来若显式启用，只能位于 Rust Host，并复用同一 DomainService。
- 不保留 HTTP/IPC 双运行栈，不做端口、Offset、执行线程、协议或压缩算法的静默 fallback。

| 重构阶段 | 当前状态 | 已有证据 | 未完成门槛 |
|---|---|---|---|
| R0 证据与测试地基 | 已完成 | 128 项问题可跟踪；65 条 API v1 路由快照；IPC v1 契约；C++ framing/queue/backpressure/shutdown harness；Rust protocol/Fake Core 测试；前端 lint/test/build 通过；Windows CI 三个 job 通过 | 目标 UE fixture 属于 R7 发布门，不再阻塞测试地基本身 |
| R1 安全止血 | 实现阶段完成；R7 验证待办 | 注入路径已止血；GameThread 使用拥有参数的 128 项有界 MPSC、单调 deadline、终态、显式取消与 drain；ProcessEvent SEH 转结构化失败；Hook restore/in-flight/unload refusal；危险 reconnect/raw transform/legacy Watch/假 WS Console 已禁用；已移除已定位的 property/DataTable/FFieldClass 猜值路径；USMAP 容器通过 C++/Rust golden consumer | Hook/注入 UE 目标 fixture；剩余硬编码布局与 offset capability 验证、目标生成 USMAP 语义验证与 dump 关闭边界 |
| R2 CoreRuntime/能力模型 | 实现阶段完成；R5/R7 验证待办 | CoreRuntime 状态机、加密随机 session 与 request lease；一次性发布的 immutable EngineContext/identity/name-layout context；带 candidate/confidence 的 offset validation report；依赖式 CapabilityRegistry；Runtime-owned GameThreadExecutor/PostRender backend；ShutdownCoordinator；SafeMemory；稳定 Handle；`EngineFacade`、immutable Snapshot store、生产 snapshot source；transport-neutral CoreCommandService；Rust Host SnapshotCache/索引；VTable Hook RAII owner 与 callback quiet drain | 其余领域 command 属于 R5；真实 identity/name/snapshot/global-pointer、GC churn、LDR/版本与目标规模证据属于 R7 |
| R3 Named Pipe/Rust Host | 实现阶段完成；R7 验证待办 | 共享严格 RPC 契约；安全且可 join 的 overlapped Named Pipe server/client；PID 核验、deadline/cancel、背压、断线、显式重连和精确 Shutdown；C++/Rust framing fuzz；真实跨语言 Core/Host 进程 fixture；EventHub、multi-PID SessionManager、PID-scoped 操作协调器与 Tauri Channel bridge；真实 x64/x86 注入矩阵 | UE 目标进程连接、Hook/GC/卸载环境矩阵属于 R7 |
| R4 通信原子切换 | 已完成 | React 领域调用只经 Tauri `domain_request`，事件只经 Tauri Channel；Rust `DomainService` 使用显式 operation registry、PID/session 绑定和稳定错误；Core release project 不编译 `Server/`/`API/` 且不链接 `ws2_32`；二进制契约确认无网络 import/legacy marker；跨语言 fixture 覆盖 DomainService -> SessionManager -> C++ Core | 无；未实现领域按 capability 明确失败，功能实现进入 R5 |
| R5 领域正确性 | 进行中（R5.1 查询切片完成） | Object/Type 查询已复用 Host SnapshotIndex，使用 generation + query fingerprint cursor、full path 身份与 1..128 有界分页；真实 C++ Core/Rust Host 两页 fixture 已通过 | Property codec、继承/CDO 语义、GC/目标规模，以及 Memory/Call/World/Watch/Hook/Blueprint/Dump 逐项验证 |
| R6 前端状态重构 | 未开始 | UI lint 已清零，基础 Vitest 已建立 | session store、查询取消、BigInt 地址、真实能力 UI |
| R7 发布硬化 | 未开始 | 无 | 性能、压力、目标 fixture、文档和发布门全部通过 |

当前不能宣称“可用”的既有功能包括：Watch/Hook producer、Console、Actor Transform 写入、属性读写、Memory/Call/World/Blueprint/Dump 等尚未迁移的领域命令、尚未由目标进程生成并完成语义验证的完整 USMAP、未完成 capability 报告的 Offset，以及未经过真实目标进程卸载 fixture 的 Hook。Host 对这些命令返回稳定的 capability 错误，不会退回旧 HTTP。旧 HTTP/SSE/WS 源码未删除，但已从 release project 和运行入口隔离。

### 0.1 R1 注入止血状态

- Rust Host 现在只接受扫描结果携带的 PID + start_time_100ns + process path 身份，执行前重新查询并拒绝 PID 复用或路径变化。
- DLL 必须 canonicalize 为普通 .dll 文件，且 PE machine 为 AMD64、optional header 为 PE32+；目标进程也必须是 x64。
- 注入权限从 PROCESS_ALL_ACCESS 收紧为 CreateThread/QueryInformation/VM Operation/Read/Write；线程直接运行，不再 suspended/resume。
- LoadLibraryW 地址按本地所属系统模块 RVA 映射到目标模块，不假定跨进程绝对地址相同；成功需要 wait signaled、exit code 非零并在目标模块表回查到同一 canonical path。
- 超时或异常 wait 不释放仍可能被远程线程读取的参数；后台清理只在线程真正结束后执行。legacy inject_dll.ps1 已明确禁用，不作为 fallback。
- Tauri `inject_and_connect` 将 DLL load、PID-scoped Pipe connected 和 Core Ready 分为独立状态；只有目标进程身份在 load 后与 Ready 后均匹配、`CoreRpcClient` 完成严格 Welcome 验证且 managed session 为 Ready 时，UI 才触发 `onCoreReady`。`already_loaded` 只是 DLL 层的幂等状态，不单独代表成功。前端已删除 `runtime.ini` 端点恢复和隐式重试；身份失配后的清理只关闭现有 Pipe transport，不向可能已复用 PID 的进程发送 Shutdown。

当前证据为 Rust 单元测试、Clippy、静态安全契约和前端 lint/test/build；真实目标进程的成功、超时、错误架构、PID 复用与重复加载 fixture 尚未执行，所以对应注入问题仍标记为 in_progress。

### 0.2 R1 Core 止血状态

- `GameThreadExecutor` 已从 API 层移入 Runtime：`IGameThreadWork` 持有命令输入/输出，`GameThreadTicket` 支持只取消 queued work 并明确区分 cancelled/running/terminal；128 项 MPSC 严格拒绝超量请求，单调 deadline 区分排队超时与运行超时，并记录排队/执行耗时。停止会取消排队任务、唤醒 waiter 并等待执行中的任务，未启用与已取消使用不同结果；所有终态都会在锁外释放 owned work，外部继续持有 ticket 不会继续持有 work 内的 `RequestLease`。已识别的 pump thread 同步等待会被 `PumpThreadWaitDenied` 拒绝且不入队，避免自阻塞。C++ exception 与 MSVC SEH 都转为 `ExecutionFailed`，不会遗留 processing 状态。`API/GameThreadQueue.h` 仅是旧 CallApi 的临时兼容适配器，不拥有第二套队列。
- 所有公开函数调用拒绝 `use_game_thread=false`；参数结构大小和字段范围必须可验证，只开放当前明确支持的标量类型，batch 上限为 64。未找到真实 ProcessEvent 时初始化失败，不存在 no-op 或 Worker 线程 fallback。
- PostRender/ProcessEvent VTable 修改会校验写保护恢复和最终指针，卸载前停止队列、恢复槽位并等待 callback in-flight 清零；任何恢复或 drain 失败都会阻止 `FreeLibraryAndExitThread`。ProcessEvent 监控改为按首次订阅延迟安装。
- 临时 HTTP Server 不再创建 detached worker。接纳计数在建线程前原子保留，所有 worker 和 socket 有 owner；`Stop()` 中断慢连接并 join 全部线程后才允许析构，监听端口严格按配置 bind，不尝试替代端口，所有写入统一走 `SendAll`。
- raw memory write 现在校验范围、`VirtualProtect`、SEH 写入和保护恢复；运行中 reconnect、raw Actor transform write、旧 Watch 和假 WebSocket Console 均明确返回 unavailable，前端也不再呈现其为已连接/实时功能。
- USMAP 容器统一由 `UExplorer::Usmap::WriteUncompressed` 写入：`None` 压缩标记、compressed/uncompressed 等长、size/open/write/flush 均受检查。生产 writer 输出与 golden fixture 完全一致，C++ 与 Rust 独立解析器均验证通过，因此确定性的“Zstd 标记 + 未压缩载荷”问题 `DUMP-001` 已关闭；真实目标生成的完整 name/enum/struct 映射仍属于 `DUMP-007` 与 R7 fixture，不能由最小空 payload 测试替代。
- Dump 启动改为单一显式线程 owner；运行中第二个任务返回 `DUMP_EXECUTOR_BUSY`，关闭等待有 5 秒边界，超时则拒绝 DLL 卸载并继续持有线程。旧 API 对任何非空 option 返回 `DUMP_OPTIONS_UNAVAILABLE`，UI 已移除未生效选项和伪 60% 进度，真实取消/阶段进度留待 R5 DumpService。
- 旧 Hook monitoring 仍含锁、JSON 和网络热路径，因此当前 capability 被硬关闭，前端无可达入口；只有无监控逻辑的 PostRender game-thread pump 保留。它必须等 R5 的预分配有界 collector、drop 指标和 Host EventHub 完成后才能重新开放。
- `CoreHarness` 已覆盖 framing、1-byte 分片、合并输入的单残帧缓存、协商 payload 上限、256 帧 × 8 种分片宽度、所有截断位置和 4096 组 deterministic header/payload mutation、加密随机 Core session、真实 Windows Named Pipe 名称/DACL/双方 PID、Hello 前空断连/残 header/残 payload/坏 magic/major/kind/flags/length、Hello/Welcome、Ready 后空闲断连、transport-neutral status/handle commands、Pipe request/ticket cancel/Ping/Pong/Shutdown drain、1024 项非阻塞 Core Event queue/writer/sequence/drop 诊断、三页 immutable snapshot 拉取、严格 cursor/limit、generation 漂移拒绝、Handle 输入拒绝/取消/lease drain、USMAP production writer/golden/独立解析、pattern 首个/跳过/末尾匹配、AMD64 PE 头与节边界、跨 64 KiB 窗口的可读节版本探测、GWorld/GEngine 唯一/重复/歧义/错误节/不稳定候选、跨页 no-access 范围、队列背压、GameThread owned work/ticket/超时/显式取消/C++ exception/SEH、生产 object/path snapshot metadata、空槽/读取失败区分、outer cycle、slot/count 变化拒绝、PostRender frame client 撤销与在途 drain、128 生产者容量、1000 次 HTTP connect/disconnect、占用端口无 fallback 和慢客户端 shutdown；Rust protocol 测试从同一 fixture 独立验证 USMAP 容器。`tests/contracts/verify-core-safety.ps1`、`tests/contracts/verify-core-runtime.ps1`、`tests/contracts/verify-host-snapshot.ps1`、`tests/contracts/verify-host-session.ps1`、`tests/contracts/verify-rpc-session.ps1`、`tests/contracts/verify-named-pipe.ps1`、`tests/contracts/verify-platform-safety.ps1` 与 `tests/contracts/verify-offset-discovery.ps1` 固化静态不变量。

本阶段最新本地证据：VS2026 `Release|x64` Core 与 `/W4 /WX` harness 构建通过；Core harness 已通过真实本机 NPFS fixture，验证规范 PID 管道名、current-user-only DACL、服务端读取后客户端 SID 校验、客户端查询服务端 PID、严格握手、全阶段断连/坏 header、领域调用、reader/worker 并行取消、Core Event、heartbeat、完整 Shutdown payload flush 与 listener/request/event writer 的 joinable stop，同时覆盖稳定 Handle、严格 snapshot page、SafeMemory、PE/pattern/offset、生产 snapshot source 与 PostRender drain。Rust protocol 10 项、Tauri Host 58 项、C++ Core/Rust SessionManager/DomainService 跨语言进程 fixture 1 项、真实注入进程矩阵 1 项、FakeCore 5 项测试及 Clippy `-D warnings` 通过；Host 的真实 Windows Pipe fixture 额外覆盖服务端 PID 在 Hello 前核验、1-byte 响应分片、Welcome header/payload 中断、Welcome 后紧随事件、Ready 空闲断连、malformed frame、并发 Ping 关联、请求、deadline/Cancel 单终态、1024 项事件队列溢出计数、溢出后的 transport drop 快照且 RPC 不阻塞、请求中断、Shutdown ack 丢失、同 PID 显式重连和 worker join。跨语言 fixture 使用真实 C++ Core event/snapshot store，通过 Host DomainService/EventHub/SnapshotCache 查询后精确关闭并要求子进程 0 退出。注入矩阵使用真实 x64/x86 子进程、真实 `CreateRemoteThread + LoadLibraryW` 和被注入的 Named Pipe DLL，覆盖成功/Core Ready、重复加载、错误 DLL/目标架构、过期 start-time 身份、`DllMain` 返回 FALSE、远程线程超时以及超时加载完成后的安全 reconnect；不使用脚本或替代注入法。12 项静态契约、release DLL 网络 import/marker 检查、`npm run lint`、8 项 Vitest 与 `npm run build` 通过。UE 目标的 FUObjectItem serial/name/snapshot、GWorld/GEngine candidate、GC churn、模块 LDR 遍历、最小化/加载期间 PostRender、Hook 恢复、受限安全描述符下的 access-denied、内存保护失败和完整目标生成 USMAP 语义验证仍是明确未执行项，不能用通用注入 fixture 代替。

### 0.3 R2 CoreRuntime 与能力模型状态

- `CoreRuntime` 现在拥有 `Created -> Initializing -> Ready/Failed -> Stopping -> Stopped` 状态、不可复制的 request lease、停止后的新请求拒绝以及 active request drain。每次载入先通过 Windows CNG `BCryptGenRandom` 生成独立的 128-bit `core-<HEX>` session；随机源失败即初始化失败，不使用固定值或弱随机 fallback。Core 先真实绑定 Named Pipe，再安装游戏 Hook；`transport.named_pipe` 只读取 listener 实况，且必须与已观测、线程稳定、未停滞的 PostRender pump 一起满足后才进入 Ready 并开放握手。旧 HTTP 单独存活不能使 Runtime Ready。
- Engine 初始化完成后构建一次 `EngineContext`，并以 `shared_ptr<const EngineContext>` 单次发布；第二次发布被拒绝。Context 固化 generation、进程/模块/对象数组身份、引擎 profile，以及每个 offset 的 value、required、source、candidate list、checks、confidence、validation state 和稳定 reason code。GWorld/GEngine 不再在多个命中中取第一个，也不再用延时变化猜测 GActiveLogWorld：候选必须位于主镜像可读写且不可执行的数据节，目标来自对象数组并通过类型检查，经过两次 checked read 后仍精确指向同一目标，且最终只能剩一个唯一 slot；否则对应 capability 明确 unavailable。ProcessEvent 的首对象 VTable 与 manual slot 也改为 SafeMemory checked read，index 必须在 `(0, 512]`，module offset 超出 `int32` 发布范围时直接失败。对象身份所需的 FUObjectItem serial、UObject、FName、UClass 与 UFunction 字段从该 Context 再捕获为不可变 `ObjectIdentityContext`，运行时 Handle 验证不再直接读取可变 `Off::*`。required offset 未验证时 context 构建直接失败。
- 名称系统初始化后会一次性捕获绝对 GNames 地址、NamePool/旧式 Chunked NameArray 种类、FName 字段、chunk/cursor、entry header/string/index 和 outline-number 布局，写入 immutable `EngineNameProfile`。结构校验通过后仍必须用 `SafeMemory` 将 index 0 精确解码为 `None`，否则 `engine.names` 以稳定 reason code 保持 unavailable。`EngineNameCodec` 运行时只读取该冻结 profile，不访问 `Off::*`、`Settings::*`、`NameArray` 或引擎 `FName::ToString`；它限制名称长度与 redirect 深度，检查全部地址运算、当前 pool cursor、旧表 entry identity，并严格验证 UTF-8/UTF-16。Harness 覆盖窄/宽字符、inline/outline number、redirect cycle、坏编码、越界、不可访问内存和两种存储布局。
- `CapabilityRegistry` 通过显式 dependency graph 推导 capability，缺失依赖不会降级执行，未定义依赖和依赖环会使构建失败。`objects.handles` 只有在命令服务、生产 identity source 与活跃稳定的 game-thread executor 同时成立时才可用；`functions.handles` 还要求 owner/path/signature 所需字段全部来自同一 validated context，`call.invoke` 再依赖该能力且仍因没有完成实际调用命令而保持 unavailable。`objects.snapshot` 只有在 production producer 实际发布完整 generation 后才可用；没有 validated name/identity profile、捕获失败或尚未完成首代时均明确 unavailable。Watch、Hook collector、Blueprint 和完整 Dump fixture 同样保持 unavailable，而不是沿用旧页面的“已支持”表述。
- `IGameThreadPump` 将执行器与 Hook 解耦，当前唯一实现 `PostRenderPumpBackend` 由 PostRender callback 驱动；执行器记录 OS thread ID、单调时钟 last tick、tick count、last task duration、queue depth/capacity 与跨线程 mismatch。Backend 还只允许一个 `IGameThreadFrameClient`，用于以每帧固定预算推进 Snapshot；detach 会先原子撤销可达指针，再通过独立 callback barrier 等待在途 client 回调，超时保留 draining owner 并允许重试，禁止析构后回调。检测到第二个 pump thread 后立即停止消费 owned work，排队任务只会超时/取消，不会在错误线程执行。`game_thread.executor` 在首次 tick 前、线程不稳定或两秒未更新时都会变为 unavailable。`status.inspect`、`status.engine` 与 `status.health` 通过领域命令暴露真实 liveness/readiness、blocker、capability map 和 pump diagnostics；旧 HTTP status 路由只是该服务的临时薄适配器。最小化/加载场景仍需真实目标 fixture，当前不会推断 PostRender 可用。
- `EngineFacade` 绑定一个 Core session、一个 `shared_ptr<const EngineContext>`、由该 Context 构造的 `EngineNameCodec` 和同 generation 的 identity source，是后续 Object/Call/World/Snapshot 活操作的单一入口；identity source 现在必须显式报告 context generation 与当前执行线程有效性，generation 漂移或非已验证 pump 线程会在读取前失败。`CoreCommandService` 不再自行临时构造 Handle/name service，而是把 owned game-thread work 交给该 facade。
- `EngineSnapshotStore` 只接受 session/context 一致、generation 严格递增、Handle 完整、index 严格有序且 name/full/class/package 元数据完整的快照；发布使用 `atomic<shared_ptr<const EngineSnapshot>>`，并发读者只能看到完整旧代或完整新代。错误 session、缺字段、乱序、超限或旧 generation 均不替换当前快照，停止后拒绝新发布。
- `EngineSnapshotCapture` 将构建拆成 `Capturing -> Validating -> Publishing` 三个阶段，每次 PostRender frame 最多处理 32 个 slot/record。捕获完成后会对包括空槽在内的整个起始范围再验证，并复查最终 object count；任一对象、空槽、元数据或 count 变化都会放弃本代并保留上一完整快照。工作记录先进入分段容器，最终 vector 也按 budget 迁移，避免一次性 O(N) 元素搬移；并发 pump 被原子 owner 拒绝，停止通过 callback barrier 阻止发布并等待 in-flight。`ObjectArraySnapshotSource` 只组合同 generation 的稳定 Handle、冻结 UObject/UClass/FName offset、严格 `EngineNameCodec` 与 `SafeMemory`，显式区分 captured/empty/failed，限制 outer 深度并拒绝 cycle，双构建 name/full/class/package/kind 后再次验证 Handle；它不直接使用 legacy UE wrapper 或可变 `Off::*`/`Settings::*`。Main 在 profile 完整时配置 producer，并通过独占 frame client 请求首代及周期刷新；关闭在 facade/source 析构前先 detach 并 drain。Harness 覆盖生产窄名 object graph、路径/类型、空槽、name/serial/count 变化、outer cycle、预算、并发 pump、运行中 drain 与原子发布。这里发布的是“完整构建并经第二遍验证的不可变 sweep”，不是引擎时钟上的瞬时原子快照；真实 GC churn 与大型对象表的帧耗时仍必须由 R7 target fixture 量化。
- `CoreCommandService` 是不依赖 HTTP 的 Core 领域边界，注册表外 operation 明确返回 `OPERATION_NOT_SUPPORTED`。status 命令只读取 immutable runtime/context/snapshot diagnostics；`objects.snapshot.page` 只接受严格的 `{cursor, limit}`，首屏 cursor 必须为 null，后续 cursor 同时绑定 snapshot generation 与最后一个 object index，generation 更新会返回 `SNAPSHOT_GENERATION_MISMATCH` 而不会混页。每页最多 128 条并返回 snapshot-wide 精确 source/record/skipped 计数；该 worker-safe 命令只持有 request lease 和 immutable `shared_ptr`，不进入游戏线程。Core 不接受 query/class/package 等过滤条件，R3 Rust Host 必须完整拉取一代后再原子发布缓存与 type/path/package/address/search 索引。`objects.handle.issue` 与 `functions.handle.issue` 只接受 `{index}`，拒绝调用方提供 address/serial/path/fingerprint，在 PostRender 执行点由生产 identity source 重新读取并签发完整 Handle。命令核验 request/session/timeout，响应模型携带 request/session、稳定错误码及 queue/execute timing；入队回调暴露 ticket，供 R3 建立 request ID 到取消句柄的唯一映射。
- Rust `uexplorer-protocol` 是 Host/FakeCore 共用的 snapshot page、record、cursor 与 Handle typed contract，`deny_unknown_fields` 防止 schema 漂移被静默接受，generation/record/name/path 上限只有一份常量来源。Host `SnapshotAssembler` 要求请求 cursor 与上一响应完全一致，并逐页核对 session/context/generation、capture metadata、精确总数、严格 index 顺序、非零 canonical address/fingerprint 和文本边界；任一页失败后 assembler 进入 terminal，缺页、混代或伪 continuation 都无法产出 `SnapshotIndex`。`SnapshotCache` 在全部页面完成和索引构建成功后才通过 `RwLock<Option<Arc<SnapshotIndex>>>` 一次替换，读者只会观察完整旧代或完整新代，且同 session 下拒绝 context 变化和非递增 generation。索引覆盖 kind、case-insensitive full/class/package path、唯一 address 和有每 record 256 token 上限的 CamelCase/token-prefix search；查询 cursor 绑定 generation、last index 和规范化 query fingerprint，精确区分 `snapshot_record_count` 与 `matched_count`。`SessionManager` 已把该缓存绑定到真实 `CoreRpcClient` transport，并按剩余 deadline 拉取 `objects.snapshot.page`；混代会最多重拉三次，只有完整 generation 才发布。当前单元测试使用同一 transport trait 的严格 Fake transport，尚不能表述为真实 UE 桌面端到端可用。
- 主关闭路径先进入 `Stopping` 并摘除命令服务入口，再由 `ShutdownCoordinator` 顺序停止旧 HTTP、等待 Dump、恢复 Hook/停止 game-thread executor、撤销并排空 Snapshot frame client、停止 facade/store，最后等待 request lease；Snapshot detach 失败时不会继续析构其 capture/source。任一 stage 不能证明安全停止都会保留 DLL，不执行 FreeLibrary。排队命令取消时会立即释放其 owned work 与 lease，即使传输层仍持有 ticket 也不会阻止 drain。Harness 覆盖缺少 Pipe 时不能 Ready、Ready 后 admission、Stopping 拒绝新请求、领域命令取消后的 lease drain、frame-client timeout/retry、stage 顺序/异常/幂等，以及 capability dependency cycle。
- `SafeMemory` 统一执行地址范围溢出检查、逐区域 `VirtualQuery`/保护状态验证、SEH 隔离读写、可写保护切换与逆序恢复。公开 `noexcept` 操作会把容器分配异常转换成稳定的 `MEMORY_ALLOCATION_FAILED`，并在修改任何页保护前预留完整 owner 容量。普通内存写不能触碰可执行页；显式代码写必须同时声明授权并刷新指令缓存。Hook VTable patch/restore 改为带 expected-value 校验的原子指针交换，不再由 API 模块直接调用 `VirtualProtect`。
- 平台扫描边界现在显式限定为 Windows x64，任何非 `_WIN64` 构建立即失败，`PLATFORM_WINDOWS32` 能力标记也已移除。`PeImageView` 使用 `SafeMemory` 验证 DOS/NT/AMD64/PE32+、镜像/header 大小、节表和每个 RVA；版本探测只以 64 KiB 有界窗口读取标记为 readable 的节，并把 probe、PE、memory、native error 分层记录。Pattern parser 事务式发布解析结果，拒绝半字节和非法 token，scanner 在分块副本上保留跨候选 `SkipCount` 并包含最后合法起点；字符串与 VTable 遍历也先通过 `SafeMemory` 读取，aligned value 的最终比较由 SEH 隔离。命名模块查找改为 nullable `GetModuleHandle`；需要遍历全部模块的路径共用有 4096 项上限、`offsetof(InMemoryOrderLinks)` 还原、`Blink` 连续性和模块范围校验的 LDR walker。合成 fixture 已验证 PE/pattern/version/range 逻辑，但 legacy sentinel lookup API 尚未完全替换，真实注入进程中的 LDR 遍历仍留给 R7。
- Memory API 采用完整消费的 `from_chars` 十六进制解析，限制单次 4096 字节与 64 级 pointer chain，checked signed offset 拒绝上溢/下溢；链中途失败返回稳定失败 envelope 和已完成 steps。typed read/write 类型现已对称且数值范围严格。未经游戏线程和 UE 语义验证的 UObject 属性写路由与前端编辑控件均已关闭，不提供静默 raw fallback。
- `ObjectHandle` 固化 `session_id + context_generation + index + serial + address + class_fingerprint`；`FunctionHandle` 额外绑定 serial-backed owner、canonical identity path 与 signature fingerprint。执行验证每次从 identity source 重读并逐字段比较；Harness 已覆盖 session/generation 过期、槽位复用、地址/类型变化及 function owner/path/signature 变化。生产 `ObjectArrayIdentitySource` 每次载入由对应 `shared_ptr<const EngineContext>` 构造，没有可重新配置的全局 singleton；它只接受 `epic_fuobjectitem_64_v1` 明确 profile，并要求 live layout serial offset 与 Context 一致：item size 必须为 `0x18/0x20`、object offset 必须为 `0`，且双读稳定、slot/InternalIndex 一致、cluster 合法并观察到正 serial；未知/加密布局不扫描猜值。对象与函数 identity 通过 `SafeMemory` 重读；函数身份路径由完整 outer 链的原始 FName comparison-index/number token 组成，不在执行边界调用未验证的显示名解码器，并绑定 serial-backed owner、反射 flags/size、exec pointer 后双读确认；所有读取拒绝非已观测 PostRender 线程。同步修正了 `index == Num()` 越界接收，以及 manual fixed/chunked 初始化未一致应用 object-table decrypt、fixed 初始化多解引用一级的问题。Handle 签发现已接入 game-thread 领域命令，object/function Handle 能力分别按真实依赖报告；该 profile 仍待真实目标 fixture，实际函数调用命令尚未实现，因此旧 HTTP call 与前端执行控件继续返回/显示 `CALL_HANDLE_REQUIRED`。
- PostRender 与 ProcessEvent 的每个 VTable slot 现在由 `VTableHookToken` 独占：token 在 CAS 前分配，安装使用 expected-original CAS，disable/enable/restore 可重试，第三方已替换 slot 时不会覆盖，恢复失败时 token 保持 active 并使关闭失败。ProcessEvent 先完整收集候选并预留 owner 容器，保证开始 patch 后不再因容器扩容留下无主 slot；面向状态接口的 installed 值来自原子诊断投影，避免并发读取 token/vector。`CallbackBarrier` 在热路径仅执行原子 enter/leave；停止后禁止 owned work，并要求 in-flight 为零且 activity sequence 在 quiet period 内不变后才释放原函数与 token。Harness 覆盖正常 RAII、外部替换、保护页导致的首次恢复失败及重试、停止后的回调和 drain。
- Core 项目显式使用 `/utf-8`，已消除 UTF-8 源文件的 C4819；本轮平台 scanner 不再经由返回 `int32` 的 legacy `StrlenHelper` 引入新的长度窄化，现有其他模块的窄化转换等 warning 仍作为 `ENG-007` baseline 保留，不能把“可构建”误写成 warnings clean。

R2 尚未完成：`FUObjectItem` identity/name/snapshot 与 GWorld/GEngine 唯一候选的真实目标 profile、GC churn/大型对象表 fixture、目标进程 LDR/版本探测 fixture、Rust SnapshotCache 经真实 Core/目标进程的规模、失效和重连验证、其余 Object/Class/Enum/Memory/World/Call/Blueprint/Dump 领域命令、Hook 热路径有界 collector/最后订阅卸载，以及剩余硬编码布局、直接读取 `Off::*`/`Settings::*`、裸指针或遍历 live UE 容器的模块仍待迁移；这些完成前不会把 R2 标记为完成。

### 0.4 R3 Named Pipe/Rust Host 状态

- `uexplorer-protocol` 现在为 Hello、Welcome、完整协商上限、Request、Response/error/timing、Event、Cancel、Heartbeat 和 Shutdown 提供唯一的 `deny_unknown_fields` Rust 类型；schema 与 golden fixture 使用相同字段。原来的 `pending_rpc`、`status.get` 和私有 FakeCore envelope 已移除，缺失或多余字段不会被静默接受。
- `FrameDecoder` 不再先复制整块 pipe read，而是一次只缓存一个未完成 frame；先解析 24-byte header 并按全局/协商上限拒绝长度，再逐步收齐 payload。Host 单次 transport read 输入硬限 64 KiB。10,000 个合并 frame 加一个残缺 header 的测试证明内部只保留最后 10 bytes，而不会按整批输入或恶意长度重复分配。
- transport-independent `CoreRpcSession` 明确执行 `Created -> HelloSent -> Ready -> Closing -> Closed`，任何 framing、JSON、envelope、版本、PID、session、capability、limit 或 correlation 违反都进入 terminal `Failed` 并清空 pending。只有与 Hello request ID 精确关联、目标 PID 匹配、选择 v1.0、声明 `engine.core`/`transport.named_pipe` 且全部限制在协议边界内的 Welcome 才能发布 Ready。
- 每个 Request/Ping 先验证协商 timeout/pending/payload 上限，再分配不超过 JSON uint53 的单调 request ID 和 deadline；显式 Cancel 与到期项从 pending map 原子迁移到最多 512 项、30 秒期限的 late-terminal tombstone，不会把迟到 Response/Pong 错配给其他调用。未知 response ID、payload/header ID 不一致、success/error/data 不变量错误均终止 session。
- Event 要求 frame request ID 为 0、`seq` 严格递增且 `dropped_before` 不回退；Ping/Pong 绑定 session、nonce、单调时间；Host Shutdown 只有收到 request ID、session 和 reason 完全一致的 acknowledgement 才进入 Closed。共享 FakeCore 已跑通完整 Hello/Request/Ping/Cancel/Shutdown 生命周期。

- Core `NamedPipeRpcServer` 已绑定 `\\.\pipe\UExplorer\v1\<pid>`，使用 `FILE_FLAG_OVERLAPPED`、`FILE_FLAG_FIRST_PIPE_INSTANCE`、`PIPE_REJECT_REMOTE_CLIENTS` 与仅当前用户 SID 的保护 DACL；读取 Hello 后再通过 `ImpersonateNamedPipeClient`/`EqualSid` 验证客户端 SID，并记录 `GetNamedPipeClientProcessId`。真实 harness 客户端同时用 `GetNamedPipeServerProcessId` 验证服务端 PID。协议 reader 每次只按已验证 header 分配 payload，并把单次 I/O 限制在 64 KiB。
- listener、4 个 request worker 和独立 Event writer 均由 `std::thread` 显式持有；每 session 最多 256 个 pending request。reader 独立于同步领域 worker，因此可在 `objects.handle.issue` 等待 PostRender 时接收 Cancel，并用同一 request ID 映射到 `GameThreadTicket`。Event producer 只进入 1024 项有界 DropOldest 队列，writer 才执行 JSON/frame/overlapped write；事件使用 uint53 sequence、64 KiB payload 上限和累计 `dropped_before`，慢 Host 不会阻塞 producer/game thread，Stop 会唤醒并 join writer。这里是 R5 Watch/Hook collector 的生产 transport adapter，不代表尚未实现的 Watch/Hook producer 已开放。worker deadline 从 frame 收到时开始计算，进入领域服务前扣除 transport queue 时间；慢 Host 只会占住有界 worker/pending 配额，不在 Hook/game thread 上执行写入。
- Main 在安装 Hook 前先完成 Pipe bind；capability 只根据真实 listener 发布，Runtime 与 PostRender readiness 同时满足后才 `OpenAdmissions`。关闭顺序先停止 Pipe、取消 ticket、唤醒 overlapped I/O 并 join listener/worker，再恢复 PostRender Hook、排空 snapshot/facade 和 request lease。Host Shutdown 必须先收到 request ID/session/reason 完全一致的回执，随后触发主关闭；未读回执通过受控 flush 保持完整，Stop 会中断同步 flush，无法排空则拒绝卸载。
- Rust `CoreRpcClient` 只打开规范 PID Pipe，使用 overlapped read/write 与 `SECURITY_IDENTIFICATION`，并在发送 Hello 前要求 `GetNamedPipeServerProcessId` 与选中 PID 精确相等；没有 TCP、`runtime.ini`、Token 或备用 Pipe 路径。一个 owned/joinable I/O worker 独占 `CoreRpcSession`，串行化状态迁移和写入；命令队列固定 256 项，transport read 固定 64 KiB，事件入口固定 1024 项，慢消费者只增加 Host drop 计数而不阻塞 reader/Core。
- Host request completion 由 request ID 精确关联，调用 deadline 从进入有界 Host command queue 时开始，worker 只把剩余毫秒数发给 Core；deadline 通过同一 session 状态机生成 Cancel，显式取消与超时都只完成调用方一次。断线会完成所有 pending 而不是悬挂；任何 worker 退出路径都会先取消并 settle 在途 overlapped read/write，再释放固定地址的 `OVERLAPPED`/buffer 并 join。精确 Shutdown 回执、服务端 PID 错配、Welcome header/payload 中断、Ready 空闲断连、malformed frame、请求中断线、Shutdown ack 丢失、Welcome 后粘连事件、1-byte 分片、并发 Ping、事件溢出以及关闭后同 PID 新连接均由真实 Windows Pipe fixture 验证。
- Host `EventHub` 每个 Core session 独立存在，最多 64 个订阅者、每订阅 1..1024 个事件、保留最近 1024 个事件；单事件序列化上限为 64 KiB，历史和订阅队列共享 `Arc<EventPayload>`，避免按订阅者复制大型 JSON。生产侧只使用 `try_send`；`CoreRpcClient` 把 transport 入口累计 drop 快照随下一个成功入队的事件传递，慢订阅者再独立累计 subscriber drop，二者合并为 `host_dropped_before`，不会阻塞 Pipe reader 或其他订阅者。事件必须满足 session、uint53 sequence/timestamp/drop、严格递增 sequence、非回退 Core/transport drop，且 sequence gap 必须由新增 Core 或 transport drop 解释；按 kind、`watch_id`、`hook_name` 精确过滤，replay 保留对应 transport drop 快照，缺失或容量不足返回明确错误而不静默截断。
- multi-PID `SessionManager` 最多管理 16 个 PID，每个 PID 有唯一连接 reservation、不可变 Welcome identity、独立 EventHub/SnapshotCache 和可 join event forwarder；只有一个显式 active session 供后续 UI 使用，但后台状态不共享。连接只调用 `CoreRpcClient::connect`，不含 HTTP/runtime.ini/token fallback。关闭先执行精确 Shutdown（失败会显式 disconnect），停止并 join event worker，再关闭订阅者；单个 session 的事件/transport 错误不会污染其他 PID。Snapshot refresh 按协商上限传递剩余 deadline、串行化同 session refresh、对 generation 变化最多重试三次，并只原子发布完整代。
- Tauri 现在以 `Arc<SessionManager>`、`Arc<TargetOperationCoordinator>` 和 `Arc<EventBridgeManager>` managed state 作为唯一 Host 生命周期根。PID-scoped RAII admission 在任何 DLL 检查/加载前拒绝同 PID 并发注入，并把全局目标操作限制为 16；同一 admission 也串行化该 PID 的显式断开，避免注入刚返回 Ready 时 session 已被并发移除。异步注入命令随后在 blocking worker 中完成 DLL load、两次 PID/start-time/path 身份核验、15 秒有界 Pipe/Welcome 等待和 active-session 发布，并以 `ready/failed + dll/pipe/core` 分层结果返回。不存在读取 `runtime.ini`、端点替换或 HTTP 重试。事件订阅通过调用方专属 `tauri::ipc::Channel<HostEvent>`，全局最多 64 个 bridge，每个 bridge 拥有 join handle、50 ms 可停止接收周期、投递失败诊断和 JavaScript-safe ID；断开 PID session 前先停止并 join 该 PID 的全部 bridge，即使一个 worker panic 也不会把其余 worker detach。
- `core_process_fixture` 启动独立的真实 C++ `CoreHarness.exe --host-session-fixture` 进程，以其真实 PID 命名 NPFS endpoint；Rust `SessionManager` 完成 peer PID/Welcome/Core Ready，消费 Core Event，分页拉取 C++ `EngineSnapshotStore` 并建立 Host 查询索引，再发送精确 Shutdown 并要求 C++ listener/request/event 线程全部 join 后进程以 0 退出。CI Rust job 必须先构建该 C++ fixture；fixture 缺失、超时或任一跨语言字段不匹配均硬失败，不 skip、不替换为 FakeCore。
- `injection_process_fixture` 构建真实 x64/x86 target EXE，以及 Ready、750 ms 慢加载、拒绝加载三种 DLL；测试直接调用生产 `inject_and_connect_target`，因此实际执行 canonical PE/identity/module 检查、最小进程权限、目标地址解析、`VirtualAllocEx`/`WriteProcessMemory`/`CreateRemoteThread`/`GetExitCodeThread`、module re-enumeration 和 `SessionManager` Welcome。矩阵要求成功后 Pipe/Core Ready，第二次调用明确 `AlreadyLoaded`，错误架构与过期 start-time 在远程写入前失败，拒绝 DLL 返回 `LOAD_LIBRARY_RETURNED_NULL`；100 ms wait 超时会返回 `dll=indeterminate` 而不谎报加载失败，资源转交 deferred cleanup，并在 750 ms load 完成后可明确 reconnect。CI 缺少任一二进制即硬失败，不执行 legacy PowerShell 注入器或其他 fallback。

R3 的协议、Core/Host transport、SessionManager、EventHub、真实跨语言 snapshot/event fixture、frame/断线矩阵与通用目标进程真实注入矩阵均已落地，R3 实现阶段完成。UE 4.26/4.27/5.x 的引擎行为、受限进程权限和完整卸载仍是 R7 目标环境门禁，不能由通用 target EXE 代替。

### 0.5 R4 通信原子切换状态

- `Dumper/Main.cpp` 只创建 PID-scoped `NamedPipeRpcServer` 和生产 `PostRenderHook`；`UExplorerCore.vcxproj` 不再编译 `Server/HttpServer.cpp` 或任何 `API/*.cpp`，也不链接 `ws2_32`。旧源文件按“不删除”约束保留，并由 `Dumper/legacy/README.md` 明确标为非运行归档。
- `frontend/src-tauri/src/services/domain_service.rs` 是桌面领域入口：先用显式白名单拒绝未知 operation，再解析显式 PID 或 active session；status 与 immutable snapshot 查询走真实 SessionManager/Core，未实现领域只返回 `CAPABILITY_UNAVAILABLE`。若 Core 错误宣称 capability 可用而 Host 无命令，则返回 `DOMAIN_COMMAND_NOT_REGISTERED`，不伪造数据。
- `frontend/src/api/client.ts` 只调用 Tauri `domain_request`/注入/session 命令；实时事件只使用 `tauri::ipc::Channel`。React 源码中已无 `fetch`、`EventSource`、`WebSocket`、localhost、Token、端口恢复、SSE polling fallback 或 `runtime.ini` 依赖。
- `verify-transport-cutover.ps1` 同时审计 Main/project/Host/React 静态边界和 release DLL import/marker；跨语言 `core_process_fixture` 通过 Rust DomainService 调用真实 C++ Core，覆盖 status、对象计数/搜索、capability unavailable 与未知 operation。Core harness 仍编译 legacy HttpServer 仅用于回归其历史生命周期，不代表 release Core 可达。
- R4 不把未实现功能包装为成功。Object/Type 仅返回 immutable snapshot 中真实存在的字段；Memory/Call/World/Watch/Hook/Blueprint/Dump 等进入 R5。当前没有外部 Gateway，也没有 HTTP/IPC 双栈兼容期。

### 0.6 R5.1 Object/Type/Snapshot 查询状态

- 本轮确认并修复了三项切换后真实可达的前端缺陷：`HierarchyPane`、`TypeBrowser`、`InstancePane` 和 `InstanceBrowser` 固定请求 500 条，而 Host 协议上限是 128，列表会稳定返回 `PAGINATION_INVALID`；continuation 失败后旧 `hasMore/cursor` 会触发重复请求；实例响应缺少 outer 时 UI 会伪造 `Package`。根因是 UI 自行复制分页/响应假设且仍沿用 offset 模型；现在统一改为最多 128 条和 Host 返回的 continuation cursor，失败会终止当前 page chain，缺失 metadata 保持缺失，不扩大上限、不静默重试也不构造占位值。
- `DomainService` 不再对 immutable snapshot 重复执行 O(N) 扫描或短名称匹配；对象、类型、包内容和类实例查询统一调用 `SnapshotIndex::query`，对象分类计数也直接读取 kind index，复用 kind/full-path/class-path/package-path/token-prefix 索引。每页返回精确 `matched/total`、`snapshot_generation/context_generation`、来源/快照记录数、`has_more` 和绑定 generation + normalized-query fingerprint 的 `next_cursor`；把游标用于另一查询会明确返回 `SNAPSHOT_QUERY_CURSOR_MISMATCH`。
- 执行身份改为完整对象路径：类型选择、详情请求、包过滤和实例过滤均传 full path；短名称只用于显示和 token-prefix 搜索。同一 full path 在 snapshot 中出现多条记录时返回 `OBJECT_IDENTITY_AMBIGUOUS`，不选择第一条。TypeScript 的 Struct/Enum/Package 项不再把缺失 full path 回退为短名称。
- package contents 从“超过 128 就失败且无法继续”改为严格的 1..128 cursor pagination；旧 `offset/q/class/package` 请求字段由 `deny_unknown_fields` 明确拒绝。跨语言 fixture 已验证真实 C++ snapshot 的连续两页保持同一 generation；Rust 单元测试另覆盖 exact package/class path、查询游标错配、重复 full path 和旧 schema 拒绝。
- 当前 R5.1 只完成查询/身份/分页切片，不能据此宣称属性反射可用。PropertyCodec、direct/inherited 语义、CDO/层级 cycle guard、GC churn、目标规模索引性能和真实 UE fixture 仍未完成；对应 capability 继续明确 unavailable。

本切片验证证据：Tauri Host 58 项单元测试（另含真实 C++ Core 进程 fixture 与真实注入进程 fixture各 1 项）、Rust protocol 10 项、FakeCore 5 项、前端 8 项 Vitest、ESLint、TypeScript/Vite production build、Clippy `-D warnings` 和 12 项静态契约均通过。

## Context

基于 Dumper-7 的实现原理，设计一个桌面端 Unreal Engine SDK Dump + 实时探索工具。
- 主要功能：SDK Dump（类似 Dumper7）
- 辅助功能：Live Explorer（类似 UE4SS / UnityExplorer，但更强大）
- 当前实现：Tauri 2 + React + TS + Vite 前端，Rust Host 领域/session 层，C++ DLL 核心（注入/劫持），Windows Named Pipe RPC 通信
- 当前边界：React/Tauri -> Rust Host -> Named Pipe -> Core DLL；DLL 不承担 HTTP/SSE/WebSocket
- 第一阶段：功能设计 + 页面功能设计（用户负责 UI 设计）

---

## 一、历史系统架构（R4 前，非当前运行路径）

> 从本节到旧 Phase 规划主要保留最初产品设计和迁移对照，其中 HTTP、SSE、
> WebSocket、Token、port 以及“已完成”描述均不是当前事实。当前边界、能力和
> 进度只以第 0 节、`REFACTOR_PLAN.md` 与 issue register 为准。

```
┌─────────────────────────────────────────────┐
│         UExplorer Desktop (Tauri 2)         │
│         React + TypeScript + Vite           │
│                                             │
│  Dashboard │ SDK Dump │ Object Browser      │
│  Class Inspector │ Function Browser         │
│  World Explorer │ Console │ Memory Viewer   │
│  Hook Manager │ Blueprint Decompiler        │
│  Settings                                   │
└──────────────────┬──────────────────────────┘
                   │ HTTP (127.0.0.1:PORT)
                   │ + SSE + WebSocket
┌──────────────────┴──────────────────────────┐
│          UExplorer Core DLL (C++)           │
│          运行在游戏进程内                      │
│                                             │
│  HTTP Server │ GObjects/GNames Discovery    │
│  Type System Resolver │ SDK Generator       │
│  Memory R/W │ ProcessEvent │ Hook Manager   │
│  Blueprint Decompiler │ World Inspector     │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────┴──────────────────────────┐
│       Target Game Process (UE 4.11-5.x)    │
└─────────────────────────────────────────────┘
```

**Core DLL 开发策略：** 直接复用 Dumper-7.Fork 源码（Engine/Generator/Platform/Utils），在此基础上新增 HTTP Server 和 API 层，适配 UExplorer 的实时交互需求。

**注入方式：**
1. DLL 劫持（Proxy DLL）— 替换游戏目录下的系统 DLL（如 xinput1_3.dll）
2. 远程注入 — Tauri 端通过 CreateRemoteThread 注入 DLL

**通信协议：**
- HTTP REST API（读操作 GET，写操作 POST）
- WebSocket 用于 Console 双向交互和高频实时数据
- SSE 用于 Hook 命中、Watch 变化等事件推送
- 共享密钥 Token 认证（`X-UExplorer-Token` header）

---

## 二、Core DLL 功能模块

### 2.1 初始化与地址发现

| 功能 | 说明 |
|------|------|
| GObjects 自动扫描 | 模式扫描 .data 段，支持 FFixedUObjectArray / FChunkedFixedUObjectArray |
| GNames 自动扫描 | 支持 FNamePool（新）和 TNameEntryArray（旧） |
| ProcessEvent 定位 | 通过签名扫描 + 字符串引用定位虚函数 |
| UE 版本检测 | 探测内存布局自动判断 4.11-5.x |
| FProperty vs UProperty | 自动检测属性系统类型（4.25 为分界线） |
| 手动偏移覆盖 | 自动扫描失败时允许用户手动输入地址 |

### 2.2 对象枚举

| 功能 | 说明 |
|------|------|
| GObjects 遍历 | 分页懒加载，支持 5 万+ 对象 |
| 对象分类 | UClass / UStruct / UEnum / UFunction / UPackage / UBlueprintGeneratedClass |
| 路径解析 | Outer 链遍历，构建完整对象路径 |
| 实时追踪 | 检测新创建/销毁的对象 |
| 多维过滤 | 按类型、包名、名称模式、标志位过滤 |

### 2.3 类型系统解析

| 功能 | 说明 |
|------|------|
| 类继承链 | 完整的父类→子类继承树 |
| 字段遍历 | PropertyLink / ChildProperties / FField 链表 |
| 属性类型解析 | 全部 UE 属性类型（见下方详细列表） |
| 函数签名 | 参数、返回值、函数标志 |
| 枚举值 | 名称-值对，底层类型 |
| 接口列表 | 类实现的所有接口 |
| 元数据 | Category, DisplayName, Tooltip 等编辑器元数据 |
| 位域解析 | BoolProperty 的 FieldMask, ByteOffset, BitIndex |

**支持的属性类型：**
- 基础类型：Bool, Byte, Int, Int64, Float, Double, Name, Str, Text
- 对象引用：Object, WeakObject, LazyObject, SoftObject, Class, SoftClass, Interface
- 容器：Array, Map, Set
- 结构体：Struct（递归解析内部结构）
- 委托：Delegate, MulticastDelegate, MulticastInlineDelegate, MulticastSparseDelegate
- 特殊：Enum（含底层类型）, FieldPath

### 2.4 SDK 生成引擎

| 输出格式 | 说明 |
|----------|------|
| C++ SDK Headers | 完整可编译 SDK，含 include、前向声明、padding、static_assert |
| USMAP Mappings | 二进制 .usmap 格式，供 FModel/UAssetAPI/CUE4Parse 使用 |
| Dumpspace JSON | 供 dumpspace.net 在线查看 |
| IDA/Ghidra Script | Python 导入脚本，创建结构体/枚举/函数签名 |
| Flat JSON | 机器可读的完整 dump，供自定义工具使用 |

**生成选项：**
- 包过滤（包含/排除指定包）
- 是否包含蓝图生成类
- Padding 风格选择
- static_assert 生成开关
- ProcessEvent 包装函数生成
- 预定义结构体覆盖（FVector, FRotator, FTransform 等）

### 2.5 内存读写

| 功能 | 说明 |
|------|------|
| 原始内存读写 | 按地址读写任意字节 |
| 类型化读写 | int, float, double, FString, FName, FText |
| 属性值读写 | 按属性名自动解析偏移并读写 |
| 容器读取 | TArray（元素类型+数量）、TMap（键值对）、TSet |
| 指针链跟踪 | 多级指针解引用，带符号解析 |
| CDO 对比 | 当前对象值 vs 类默认对象值的差异 |

### 2.6 ProcessEvent / 函数调用

| 功能 | 说明 |
|------|------|
| 调用任意 UFunction | 前端发送 JSON 参数，DLL 构建参数结构体并调用 |
| 静态函数调用 | 通过 CDO 调用 |
| 批量调用 | 对多个对象调用同一函数 |
| 返回值提取 | 自动反序列化返回值和 out 参数 |

### 2.7 Hook 管理器

| 功能 | 说明 |
|------|------|
| UFunction Hook | Pre/Post ProcessEvent 拦截 |
| 调用日志 | 记录参数和返回值 |
| 条件 Hook | 仅在满足条件时记录 |
| 调用频率统计 | 计数和频率追踪 |
| 实时推送 | 通过 SSE 推送 Hook 命中事件到前端 |

### 2.8 蓝图反编译

| 功能 | 说明 |
|------|------|
| 字节码反汇编 | 原始字节码列表 |
| 伪代码反编译 | 可读的 C++ 风格伪代码 |
| 控制流重建 | if/else, for, while, switch |
| 局部变量解析 | 识别并命名局部变量 |

### 2.9 世界与 Actor 检查

| 功能 | 说明 |
|------|------|
| UWorld / ULevel 枚举 | 当前世界和所有加载的关卡 |
| Actor 列表 | 按关卡分组，含类名、名称、Transform |
| 组件层级 | 每个 Actor 的组件树 |
| Transform 编辑 | 实时修改位置/旋转/缩放 |
| 快捷访问 | GameMode, GameState, PlayerController, Pawn |
| 流关卡状态 | Streaming Level 加载状态 |

---

## 三、页面功能设计（6 页精简版）

```
┌────────────────────────────────────────────────────────────────────────┐
│  顶部导航栏：Dashboard │ Objects │ Functions │ Memory │ SDK Dump │ Settings
└────────────────────────────────────────────────────────────────────────┘
```

---

### Page 1: Dashboard（首页/仪表盘）

**定位：** 快速总览和快捷入口

**覆盖 DLL API：** Status API

**功能区域：**

1. **连接状态栏**（顶部）
   - 连接指示灯（已连接绿色/断开红色）
   - 游戏进程：进程名、PID、架构（x64）
   - UE 版本号（如 UE 4.26.2）
   - GObjects 数量

2. **核心统计卡片**（中间）
   - Classes 数量（可点击跳转到 Objects 页面，类型过滤为 Class）
   - Structs 数量（跳转到 Objects，类型过滤为 Struct）
   - Enums 数量（跳转到 Objects，类型过滤为 Enum）
   - Functions 数量（跳转到 Functions 页面）
   - Packages 数量（跳转到 Objects，类型过滤为 Package）
   - Actors 数量（跳转到 Objects，类型过滤为 Actor）

3. **快捷操作区**（底部）
   - 一键生成 SDK（弹出格式选择：C++/USMAP/Dumpspace/IDA）
   - 打开对象浏览器
   - 打开函数浏览器
   - 打开内存工具

---

### Page 2: Objects（对象浏览器）— Crystal IDE 三面板架构

**定位：** 所有对象/类/结构体/枚举/Actor 的查看和运行编辑

**覆盖 DLL API：** Objects API、Classes API、Enums API、Memory API（读写属性）

**架构：** 借鉴现代 IDE 的三面板流式布局，移除了旧版的 Tab 切换，实现统一的探索体验：

**文件结构：**
- `Objects.tsx` — 整体容器层与 Flexbox 骨架（~90 行）
- `objects/HierarchyPane.tsx` — 面板 1: 类/结构体/枚举继承树与虚拟列表搜索
- `objects/InstancePane.tsx` — 面板 2: 选中类的实时实例网格（Grid）列表
- `objects/InspectorPane.tsx` — 面板 3: 实时属性观察器、字段遍历、函数查看与内存数值编辑

**功能交互流：**
- **层级发现 (Pane 1):** 用户在左侧面板按分类过滤并搜寻目标 `Class`。选用 `@tanstack/react-virtual` 呈现超大列表。
- **实例追踪 (Pane 2):** 选中 `Class` 后，中间面板实时加载对应类的生还（Live）实例，展示它们的在内存中的 Address。
- **属性探查 (Pane 3):**
  - 未选定实例、仅选定类时，展示该类的内存蓝图结构（父类继承、C++ 字段定义、函数暴露情况）。
  - 选择具体实例后，化身内存修改器，罗列其全部 `FProperty` 的当前数值。提供 Crystal UI 风格的表单支持用户在线反写数值。


---

### Page 3: Functions（函数浏览器）

**定位：** 函数查看、调用、Hook、蓝图反编译

**覆盖 DLL API：** Call API、Hook API、Blueprint API、Watch API

**功能区域：**

1. **搜索与过滤栏**（顶部）
   - 搜索框（支持 `ClassName::FuncName` 格式）
   - 标志过滤：Native / BlueprintCallable / BlueprintEvent / Static / Exec
   - 类过滤器

2. **左侧面板 — 函数列表**
   - 显示：`ClassName::FunctionName`
   - 标志图标（N=Native, B=Blueprint, S=Static）
   - 总数显示
   - 可滚动，支持大量函数

3. **右侧面板 — 函数详情**（多 Tab 切换）
   - **Info Tab**：完整名称、Flags、参数数量、返回类型、ParamSize、函数地址、字节码大小
   - **Parameters Tab**：参数列表
     - 列：名称、类型、方向（In/Out/Return）、Flags
   - **Call Tab**：函数调用器
     - 目标对象选择器（搜索名称/路径/地址，或从 Objects 页面选取）
     - 参数输入表单（根据函数签名自动生成，类型感知输入控件）
     - 执行按钮
     - 返回值显示区
     - 调用历史（参数 + 返回值记录）
   - **Hook Tab**：Hook 控制
     - Hook 开关（启用/禁用）
     - 日志模式开关
     - 命中次数统计
     - Hook 日志查看器
       - 实时滚动（SSE 推送）
       - 每条记录：时间戳、调用者对象、参数值、返回值
       - 过滤/暂停按钮
   - **Decompile Tab**（仅蓝图函数）：蓝图反编译
     - 字节码视图：偏移、操作码、操作数、注释
     - 伪代码视图：C++ 风格，控制流结构（if/else, for, while, switch）
     - 语法高亮
     - 复制按钮

---

### Page 4: Memory（内存工具）

**定位：** 内存查看/编辑 + 命令执行 + 属性监视

**覆盖 DLL API：** Memory API、Watch API

**功能区域：**

1. **地址导航栏**（顶部）
   - 地址输入框（支持十六进制和符号名）
   - 刷新按钮
   - 前进/后退按钮
   - 书签下拉菜单

2. **中间 — 十六进制视图**
   - 经典布局：地址 | 十六进制字节 | ASCII
   - 每行 16 字节
   - 可配置每行字节数
   - 点击字节可编辑（调用 Memory Write API）
   - 高亮修改过的字节

3. **右侧 — 类型化解读面板**
   - 当前光标位置的多类型解读
   - 类型：int8/16/32/64, uint8/16/32/64, float, double, pointer
   - FString / FName / FText 智能识别
   - 结构体叠加：选择结构体类型，按字段着色显示

4. **指针链工具**（独立面板）
   - 基址输入框
   - 偏移链输入（逗号分隔）
   - 逐级显示：每步的地址和值
   - 最终地址的类型化读取
   - 跳转按钮

5. **底部 — Console 面板**
   - 命令输入框（支持多行）
   - 输出日志区（滚动）
   - 自动补全（对象名、类名、函数名、属性名）
   - 命令历史（上下箭头）
   - **内置命令：**
     - `get <path_or_address>` — 获取对象信息
     - `set <object>.<property> <value>` — 写入属性值
     - `call <object> <function> <args...>` — 调用函数
     - `watch <object> <property>` — 添加监视
     - `unwatch <id>` — 移除监视
     - `instances <class>` — 列出类的实例
     - `mem.read <address> <size>` — 读内存
     - `mem.write <address> <bytes>` — 写内存

6. **右侧边栏 — Watch 监视面板**
   - 监视列表表格
     - 列：ID、对象、属性、当前值、变化时间
   - 值变化高亮（绿色）
   - 删除按钮
   - 跳转到对象按钮

---

### Page 5: SDK Dump（SDK 生成中心）

**定位：** 生成 SDK/映射文件

**覆盖 DLL API：** Dump API

**功能区域：**

1. **格式选择器**（顶部 Tab）
   - C++ SDK
   - USMAP
   - Dumpspace JSON
   - IDA Script

2. **选项面板**（左侧）
   - 包过滤器
     - 包含模式：仅列出指定包
     - 排除模式：排除指定包
     - 输入框带自动补全
   - 蓝图类开关：是否包含 BlueprintGeneratedClass

3. **C++ SDK 专属选项**（展开面板）
   - Padding 风格：`uint8 UnknownData_OFFSET[N]` / `char pad[N]`
   - static_assert 开关（偏移验证 + 大小验证）
   - 命名空间名称
   - 预定义结构体覆盖（FVector, FRotator, FTransform 等）

4. **生成面板**（右侧）
   - 大按钮：开始生成
   - 进度条 + 百分比
   - 当前处理的包名
   - 预计剩余时间

5. **任务历史**（底部折叠面板）
   - 列表：序号、格式、状态、类数量、开始时间、耗时
   - 操作：重新生成、打开输出目录

6. **输出**
   - 文件保存到本地目录
     - C++ SDK → `CppSDK/`
     - USMAP → `Mappings/`
     - Dumpspace → `Dumpspace/`
     - IDA Script → `IDAMappings/`
   - 完成后弹出文件管理器

---

### Page 6: Settings（设置）

**定位：** 全局配置

**功能区域：**

1. **连接设置**
   - HTTP 端口（默认自动分配，27015-27020）
   - Token 密钥（用于 X-UExplorer-Token 认证）
   - 自动重连开关

2. **DLL 设置**
   - DLL 文件路径（浏览按钮）
   - 注入方式：
     - 劫持 DLL：目标 DLL 名称（xinput1_3.dll / version.dll / winhttp.dll）
     - 远程注入：注入方法选择
   - 自动注入开关

3. **Dump 默认设置**
   - 默认输出目录
   - 默认格式选择

4. **显示设置**
   - 主题：亮色 / 暗色
   - 地址格式：0x 前缀 / 无前缀
   - 数值格式：十六进制 / 十进制

5. **手动偏移覆盖**（展开面板）
   - GObjects 地址（手动输入，用于自动扫描失败时）
   - GNames 地址
   - ProcessEvent 偏移
   - 测试连接按钮

6. **关于**
   - 版本号
   - GitHub 链接

---

## 四、Legacy HTTP API 设计（历史快照，非当前运行路径）

> 本章记录 R4 前的 API v1 表面，供迁移对照和 contract drift 检查。
> Release Core 不编译这些适配器；当前桌面调用以 `DomainService` operation registry
> 和 `protocol/v1` 为准，不得从本章恢复 HTTP/SSE/WebSocket 或 Token/port 配置。

### 4.1 协议约定

```
Base URL:    http://127.0.0.1:{PORT}/api/v1
认证:        X-UExplorer-Token: {shared_secret}
请求格式:    JSON (Content-Type: application/json)
响应格式:    JSON (UTF-8)
实时推送:    GET /api/v1/events/stream (SSE)
双向交互:    WS /api/v1/ws/console (WebSocket)
```

**标准响应信封：**
```json
{
  "success": true,
  "data": { ... },
  "error": null,
  "timestamp": 1709312041,
  "duration_ms": 12.5
}
```

**分页约定：**
```
?offset=0&limit=50&sort=name&order=asc
```
```json
{
  "items": [...],
  "total": 58432,
  "offset": 0,
  "limit": 50
}
```

### 4.2 API 端点一览

#### 状态与连接

| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/status` | DLL 状态、UE 版本、地址、对象数量 |
| GET | `/status/health` | 心跳检测 |
| GET | `/status/engine` | 详细引擎信息 |
| POST | `/status/reconnect` | 重新扫描 GObjects/GNames |

#### 对象枚举

| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/objects` | 分页对象列表（支持 filter/sort） |
| GET | `/objects/:index` | 按 GObjects 索引获取单个对象 |
| GET | `/objects/search?q=&class=&package=` | 多条件搜索 |
| GET | `/objects/count` | 按类型统计数量 |
| GET | `/objects/:index/properties` | 对象的所有属性值 |
| GET | `/objects/:index/outer-chain` | 完整 Outer 链 |
| GET | `/objects/by-address/:addr` | 按内存地址查找 |
| GET | `/objects/by-path/:path` | 按完整路径查找 |

#### 类型系统

| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/classes` | 分页类列表 |
| GET | `/classes/:name` | 类完整信息（字段、函数、继承） |
| GET | `/classes/:name/hierarchy` | 继承树（父类+子类） |
| GET | `/classes/:name/fields` | 所有字段（含类型、偏移、大小、标志） |
| GET | `/classes/:name/functions` | 所有函数签名 |
| GET | `/classes/:name/instances` | 该类的实时实例列表 |
| GET | `/classes/:name/cdo` | CDO 属性值 |
| GET | `/structs` | 分页结构体列表 |
| GET | `/structs/:name` | 结构体完整信息 |
| GET | `/enums` | 分页枚举列表 |
| GET | `/enums/:name` | 枚举值列表 |
| GET | `/packages` | 包列表 |
| GET | `/packages/:name/contents` | 包内所有类型 |

#### SDK 生成

| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/dump/sdk` | 生成 C++ SDK（返回 job ID） |
| POST | `/dump/usmap` | 生成 USMAP |
| POST | `/dump/dumpspace` | 生成 Dumpspace JSON |
| POST | `/dump/ida-script` | 生成 IDA/Ghidra 导入脚本 |
| GET | `/dump/jobs/:id` | 查询任务状态和进度 |

#### 内存操作

| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/memory/read` | 读取原始字节 `{addr, size}` |
| POST | `/memory/write` | 写入原始字节 `{addr, bytes}` |
| POST | `/memory/read-typed` | 类型化读取 `{addr, type}` |
| POST | `/memory/write-typed` | 类型化写入 `{addr, type, value}` |
| GET | `/objects/:index/property/:name` | 读取单个属性值 |
| POST | `/objects/:index/property/:name` | 写入单个属性值 |
| POST | `/memory/pointer-chain` | 指针链跟踪 `{base, offsets[]}` |

#### 属性监视

| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/watch/add` | 添加监视 `{object_index, property, interval_ms}` |
| DELETE | `/watch/:id` | 移除监视 |
| GET | `/watch/list` | 所有活跃监视 |
| GET | `/watch/:id/history` | 值变化历史 |

#### 函数调用

| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/call/function` | 调用 UFunction `{object_index, function_name, params{}}` |
| POST | `/call/static` | 通过 CDO 调用静态函数 |
| POST | `/call/batch` | 批量调用 `{object_indices[], function_name, params{}}` |

#### Hook 管理

| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/hooks/add` | 添加 Hook `{function_path, condition?}` |
| DELETE | `/hooks/:id` | 移除 Hook |
| PATCH | `/hooks/:id` | 启用/禁用 Hook |
| GET | `/hooks/list` | 所有活跃 Hook |
| GET | `/hooks/:id/log` | Hook 调用日志 |

#### 蓝图反编译

| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/blueprint/:funcpath/bytecode` | 原始字节码反汇编 |
| GET | `/blueprint/:funcpath/decompile` | 伪代码反编译 |

#### 世界与 Actor

| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/world` | 当前 UWorld 信息 |
| GET | `/world/levels` | 所有已加载关卡 |
| GET | `/world/actors` | 分页 Actor 列表（支持过滤） |
| GET | `/world/actors/:index` | Actor 详情（组件、Transform） |
| POST | `/world/actors/:index/transform` | 修改 Actor Transform |
| GET | `/world/actors/:index/components` | 组件层级 |
| GET | `/world/shortcuts` | 快捷访问：GameMode, GameState, PC, Pawn |

#### 实时事件流

| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/events/stream` | 统一 SSE 流（Hook 命中、Watch 变化、日志） |
| GET | `/events/watches` | 仅 Watch 变化的 SSE 流 |
| GET | `/events/hooks` | 仅 Hook 命中的 SSE 流 |
| WS | `/ws/console` | WebSocket — Console 双向交互（命令发送 + 结果推送） |
| WS | `/ws/events` | WebSocket — 统一实时事件流（替代 SSE，适合高频场景） |

---

## 五、项目文件结构（当前状态）

```
D:\Projects\UExplorer\
├── DESIGN.md                          # 本文档
├── UExplorerCore.slnx                 # VS2026 解决方案文件
│
└── Dumper\                            # Core DLL 项目
    ├── UExplorerCore.vcxproj          # MSBuild 项目文件 (v145, C++latest)
    ├── Main.cpp                       # DLL 入口点 (DllMain → MainThread)
    │
    ├── Server\                        # HTTP 服务器层
    │   ├── HttpServer.h               # PIMPL 接口 (HttpRequest/HttpResponse/RouteHandler)
    │   └── HttpServer.cpp             # WinSock2 实现 (路由匹配/Token认证/CORS/SSE/WebSocket)
    │
    ├── API\                           # REST API 路由层
    │   ├── ApiCommon.h                # JSON 响应信封 (MakeResponse/MakeError/ParseQuery/GetPathSegment)
    │   ├── Router.h / Router.cpp      # 路由注册中心 (RegisterAllRoutes)
    │   ├── StatusApi.h / .cpp         # GET /status,/status/health,/status/engine + POST /status/reconnect
    │   ├── ObjectsApi.h / .cpp        # GET /objects/*, /objects/:index/outer-chain, /objects/:index/property/:name, /packages/*
    │   ├── ClassesApi.h / .cpp        # GET /classes, /classes/:name, fields, functions, hierarchy, /structs
    │   ├── EnumsApi.h / .cpp          # GET /enums, /enums/:name
    │   ├── DumpApi.h / .cpp           # POST /dump/sdk,usmap,dumpspace,ida-script + GET /dump/jobs
    │   ├── MemoryApi.h / .cpp         # POST /memory/read, /memory/write, /memory/read-typed, /memory/write-typed, /memory/pointer-chain
    │   ├── CallApi.h / .cpp           # POST /call/function, /call/static, /call/batch
    │   ├── WorldApi.h / .cpp          # GET /world, /world/levels, /world/actors/* + POST /world/actors/:index/transform
    │   ├── WatchApi.h / .cpp          # /watch/add, /watch/list, /watch/:id/history, DELETE /watch/:id
    │   ├── HookApi.h / .cpp           # /hooks/add, /hooks/list, /hooks/:id/log, PATCH/DELETE
    │   ├── BlueprintApi.h / .cpp      # /blueprint/:funcpath/{bytecode,decompile} + ?index 兼容
    │   └── EventsApi.h / .cpp         # SSE 路由注册 + 事件广播
    │
    ├── Engine\                        # [Dumper7] UE 引擎抽象层
    │   ├── Public\Unreal\             #   ObjectArray, NameArray, UnrealObjects, UnrealTypes, Enums
    │   ├── Public\OffsetFinder\       #   自动偏移扫描
    │   ├── Public\Blueprint\          #   蓝图反编译器
    │   └── Private\                   #   对应实现文件
    │
    ├── Generator\                     # [Dumper7] SDK 生成器
    │   ├── Public\Generators\         #   CppGenerator, MappingGenerator, DumpspaceGenerator, IDA
    │   ├── Public\Managers\           #   Package/Struct/Enum/Member/Collision/Dependency Manager
    │   ├── Public\Wrappers\           #   Enum/Struct/Member Wrapper
    │   └── Private\                   #   对应实现文件
    │
    ├── Platform\                      # [Dumper7] 平台层
    │   ├── Public\                    #   Architecture.h, Platform.h
    │   └── Private\                   #   Arch_x86.cpp, PlatformWindows.cpp
    │
    ├── Utils\                         # [Dumper7] 工具库
    │   ├── Json\json.hpp              #   nlohmann/json
    │   ├── Compression\zstd.*         #   zstd 压缩
    │   ├── Dumpspace\DSGen.*          #   Dumpspace 生成
    │   └── Encoding\                  #   Unicode/UTF 编码工具
    │
    ├── Settings.h / Settings.cpp      # [Dumper7] 配置管理
    └── TmpUtils.h                     # [Dumper7] 临时工具函数
```

---

## 六、历史开发阶段规划（R0-R7 之前的记录）

> 本章的“已完成”只表示旧实现曾存在。当前进度只以第 0 节、
> `REFACTOR_PLAN.md` 和 `docs/issue-status.json` 的验证状态为准。

### Phase 1: 基础框架（已完成）

**目标：** 功能设计 + UI 设计 + 项目脚手架

- [x] 功能设计文档（本文档）
- [ ] UI 设计（用户负责）
- [ ] Tauri 2 + React + TS + Vite 项目初始化
- [ ] 前端路由和页面骨架
- [x] Core DLL 项目结构搭建（MSBuild .vcxproj + .slnx，VS2026）
- [x] Dumper7 源码复用（Engine/Generator/Platform/Utils 全量引入）
- [x] HTTP 通信层基础（WinSock2 HTTP Server，支持路由、Token 认证、CORS）
- [x] DLL 入口点（Main.cpp：初始化引擎 → 启动 HTTP Server → F6 卸载）
- [x] 基础 API 路由框架（Router + ApiCommon 响应信封）
- [x] 状态 API（/status, /status/health）
- [x] 对象枚举 API 基础（/objects, /objects/count）
- [x] 首次编译通过，生成 UExplorerCore.dll（1008KB）
- [x] DLL 注入验证通过（Wandering_Sword, UE 4.26.2, 134206 对象）
- [ ] 前端 HTTP Client 封装（axios/fetch + 类型定义）

### Phase 2: 核心 Dump 功能（已完成）

**目标：** 实现主要的 SDK Dump 能力

- [x] DLL 初始化：GObjects / GNames 自动扫描（复用 Dumper7 的 Generator::InitEngineCore）
- [x] UE 版本检测 + FProperty/UProperty 自动判断（复用 Dumper7 的 OffsetFinder）
- [x] 对象枚举 API（分页+过滤+搜索，134206 对象验证通过）
- [x] 类型系统解析 API — 类（/classes, /:name, /fields, /functions, /hierarchy）
- [x] 类型系统解析 API — 结构体（/structs, /:name）
- [x] 类型系统解析 API — 枚举（/enums, /:name 含枚举值）
- [x] C++ SDK 生成器 API（后台任务，2.6s 完成，输出到 CppSDK/）
- [x] USMAP 生成器 API（后台任务，247ms 完成，输出到 Mappings/）
- [x] Dumpspace 生成器 API（后台任务，2.7s 完成，输出到 Dumpspace/）
- [x] IDA 映射生成器 API（后台任务，40ms 完成，输出到 IDAMappings/）
- [x] Dump 任务管理 API（/dump/jobs, /dump/jobs/:id 状态查询）
- [x] Dashboard 页面（Tauri + React）
- [x] Tauri 应用构建成功（exe: 8.1MB）
- [x] SDK Dump Center 页面 — 前端（已实现，见 `frontend/src/pages/SDKDump.tsx`）
- [ ] DLL 注入/劫持机制（Tauri 端）

### Phase 3: Explorer 基础（当前阶段）

**目标：** 实现实时对象浏览和类型检查

- [x] Object Browser 页面（支持 Class/Struct/Enum 列表嵌套展开与详细属性查看）— 前端（已实现，见 `frontend/src/pages/Objects.tsx`）
- [ ] Class Inspector 页面（继承树 + 字段/函数详情）— 前端（部分实现：字段/函数/实例已在 Objects 页；继承树与独立页面未完成）
- [x] 内存读取 API — `POST /memory/read`, `/memory/read-typed`, `/memory/pointer-chain`
- [x] 属性值读取（按名称自动解析偏移）— `GET /objects/:index/properties`，支持 Bool/Int/Float/Double/FName/FString/Object/Array 等
- [x] 单对象详情 API — `GET /objects/:index`，含 outer chain
- [x] Blueprint 反编译偏移修复 — Script offset fallback 机制
- [x] 基础 Console 页面 — 前端（已实现，集成在 `frontend/src/pages/Memory.tsx` 底部）

### Phase 4: 高级 Explorer

**目标：** 实现运行时交互能力（DLL 端）

- [x] 内存写入 + 属性值编辑 — `POST /memory/write`, `POST /objects/:index/property/:name`
- [x] ProcessEvent 函数调用（含静态/批量）— `POST /call/function`, `/call/static`, `/call/batch`
- [x] 属性监视（Watch）功能（含历史）— `/watch/add`, `/watch/list`, `/watch/:id/history`, SSE 实时推送
- [x] Function Browser 页面（含调用器）— 前端（已实现，见 `frontend/src/pages/Functions.tsx`）
- [x] World Explorer API — `/world`, `/world/levels`, `/world/actors`, `/world/actors/:index`, `/world/actors/:index/components`, `/world/actors/:index/transform`, `/world/shortcuts`
- [x] Memory Viewer 页面 — 前端（已实现，见 `frontend/src/pages/Memory.tsx`）

### Phase 5: 进阶功能

**目标：** Hook、蓝图反编译等高级特性（DLL 端）

- [x] Hook Manager（UFunction Hook + SSE 实时推送）— `/hooks/*`, SSE 推送
- [x] WebSocket 实时通道（基础可用）— `WS /ws/console`, `WS /ws/events`
- [x] Hook Manager 页面 — 前端（已实现：功能已集成在 Functions 页 Hook Tab，并完成界面重构）
- [x] Blueprint Decompiler — `/blueprint/:funcpath/bytecode`, `/blueprint/:funcpath/decompile`（并兼容 `?index=`）
- [x] Blueprint Decompiler 页面 — 前端（已实现：功能已集成在 Functions 页 Decompile Tab，并完成界面重构）
- [x] IDA/Ghidra 导入脚本生成 — `/dump/ida-script`
- [x] Dumpspace JSON 生成 — `/dump/dumpspace`

### Phase 6: 代码审计与 i18n（当前阶段）

**目标：** 全面审计 C++ 后端安全问题，前端全量国际化

- [x] C++ 后端审计（39 个问题：7 Critical, 12 High, 11 Medium, 9 Low）
- [x] MemoryApi SEH 保护（所有内存读写加 IsBadReadPtr + __try/__except）
- [x] Main.cpp WINAPI 调用约定修复 + double fclose 修复 + Kismet null check
- [x] GameThreadQueue 竞态条件修复（锁内拷贝字段 + 等待槽位释放）
- [x] HookApi 性能优化（shared_mutex 替代 mutex，读路径无阻塞）
- [x] EventsApi 悬挂指针修复（mutex 保护 g_Server + SetServer(nullptr) in shutdown）
- [x] WatchApi SEH 保护 + 父类属性搜索
- [x] ApiCommon 安全解析助手（SafeParseInt/SafeParseUInt64）
- [x] 翻译系统全面扩展（300+ 翻译键，覆盖所有页面文本）
- [x] Settings 页面语言切换功能（中文/英文）
- [x] 清除所有 .tsx 中的硬编码中文字符串
- [x] 前端全页面 i18n 完善 — Functions/Memory/SDKDump/Objects/InspectorPane/InstancePane/HierarchyPane 所有可见文本用 t() 包裹
- [x] Memory.tsx CSS 拼写修复（`p-2(` → `p-2 `）
- [x] TypeScript 编译零错误验证通过
- [x] C++ Medium/Low 审计全量修复（17 项）— HttpServer 安全加固、ObjectsApi FString SEH、TryFindProperty 父类搜索、WritePropertyValue 存活检查、DumpApi 线程管理、CallApi 参数安全、代码去重、WorldApi 线程安全
- [x] 前端功能对接 — InspectorPane: Copy Address + Watch Object + 属性保存反馈 + 继承链面包屑 + CDO 默认值标签页
- [x] Objects 面板拖拽调整大小 + 真实连接状态检测
- [x] Memory Console 自动滚动
- [x] HierarchyPane Package 子标签（对接 getPackages API）
- [x] getClassHierarchy / getClassCDO / getPackages 全部对接到前端

### 页面实现状态总览（重点 8 项）
1. SDK Dump Center 页面：已实现（独立页）。
2. Object Browser 页面：已实现（独立页，支持多类型嵌套结构展开与上下文内容级联渲染）。
3. Class Inspector 页面：部分实现（能力在 Objects 页面，未独立拆页，继承树未完成）。
4. 基础 Console 页面：已实现（Memory 页面底部 Console）。
5. Function Browser 页面（含调用器）：已实现（独立页）。
6. Memory Viewer 页面：已实现（独立页）。
7. Hook Manager 页面：已实现（集成在 Functions 页 Hook Tab，已完成高密度 Crystal IDE 风格重构）。
8. Blueprint Decompiler 页面：已实现（集成在 Functions 页 Decompile Tab，已完成高密度 Crystal IDE 风格重构）。

---

## 七、与 Dumper7 的关键差异

| 维度 | Dumper7 | UExplorer |
|------|---------|-----------|
| UI | 无（控制台输出） | 完整桌面 GUI |
| 交互方式 | 一次性 Dump 后退出 | 持续运行，实时交互 |
| 对象浏览 | 仅 Dump 到文件 | 实时浏览、搜索、过滤 |
| 属性编辑 | 不支持 | 支持实时读写 |
| 函数调用 | 仅内部使用 ProcessEvent | 暴露给用户，可调用任意函数 |
| Hook | 不支持 | 支持 UFunction Hook + 日志 |
| 蓝图反编译 | 有但仅输出到文件 | 交互式查看，语法高亮 |
| 世界检查 | 不支持 | Actor 浏览、Transform 编辑 |
| 输出格式 | 4 种 | 5+ 种 |
| 通信 | 无（同进程） | HTTP + WebSocket，前后端分离 |

---

## 八、技术选型

| 组件 | 方案 |
|------|------|
| 前端框架 | Tauri 2 + React 18 + TypeScript + Vite |
| 状态管理 | Zustand 或 Jotai |
| HTTP Client | axios + React Query（自动缓存/重试） |
| SSE Client | EventSource API 原生 |
| WebSocket Client | 原生 WebSocket API |
| 代码高亮 | Monaco Editor 或 Prism.js |
| 虚拟滚动 | @tanstack/react-virtual |
| 十六进制视图 | 自定义组件（canvas 或虚拟滚动） |
| DLL HTTP Server | WinSock2（纯 Windows API，零外部依赖） |
| DLL JSON | nlohmann/json（已随 Dumper7 源码引入，Utils/Json/json.hpp） |
| DLL 构建 | MSBuild .vcxproj + .slnx（Visual Studio 2026, PlatformToolset v145） |

---

## 九、验证方案

1. **DLL 通信验证：** DLL 注入后，前端 Dashboard 能显示连接状态、UE 版本、对象数量
2. **Dump 功能验证：** 对已知 UE 游戏生成 C++ SDK，与 Dumper7 输出对比验证正确性
3. **Object Browser 验证：** 能浏览 5 万+ 对象，搜索响应 < 200ms，属性值与 Cheat Engine 交叉验证
4. **函数调用验证：** 通过 Console 调用 `GetGameName()` 等已知函数，验证返回值正确
5. **Hook 验证：** Hook `Tick` 函数，验证 SSE 实时推送到前端
