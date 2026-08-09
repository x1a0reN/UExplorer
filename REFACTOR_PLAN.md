# UExplorer 全面重构计划

> 文档日期：2026-08-09
>
> 审计范围：当前工作区的 Core DLL、Engine/Generator、HTTP/SSE/WebSocket、Tauri Host、React 前端、构建配置与项目文档。
>
> 证据边界：本文区分“已复现”“静态确认”和“待目标进程验证”。当前没有可控目标游戏进程，因此不能把跨 UE 版本的运行效果表述为已验证。

## 1. 结论先行

当前项目的产品方向没有根本错误：把 Dumper-7 的 UE 反射和生成能力做成可交互桌面工具是成立的，React + Tauri 也适合作为桌面 UI。真正的问题是**进程内 Core 承担了过多职责**：它同时做 UE 内存访问、Hook、任务调度、HTTP 解析、认证、SSE/WS 长连接和 JSON 广播。这样会把网络慢客户端、协议解析错误和线程退出问题直接放大成目标游戏崩溃或 DLL 卸载后的 UAF。

目标架构确定为：

```text
React UI
   | Tauri invoke / Tauri event
   v
Rust Host
   |- SessionManager / InjectionService
   |- DomainService / EventHub / JobManager
   |- 可选的 localhost HTTP + WebSocket Gateway（供 curl/外部工具）
   |  所有入口复用同一套 DomainService，不复制业务逻辑
   v
Windows Named Pipe RPC
   v
Core DLL
   |- CoreRuntime / CapabilityRegistry
   |- EngineFacade / SnapshotService
   |- GameThreadExecutor
   |- HookCollector / WatchScheduler
   `- DumpExecutor
```

核心决策：

1. **Core DLL 不再提供 HTTP、SSE 或 WebSocket。** DLL 只保留 UE 相关的最小能力和 Named Pipe RPC。
2. **React 不再直连 DLL。** 桌面 UI 只通过 Tauri command/event 与 Rust Host 通信。
3. **外部 HTTP 能力可以保留，但必须位于 Rust Host。** 它是同一领域服务的适配器，不是第二套业务实现。
4. **不提供运行时 fallback。** IPC 不可用就明确失败；固定端口不可用就明确失败；压缩配置为 Zstd 就必须真正生成 Zstd；游戏线程泵不可用就禁止调用，而不是切到 HTTP Worker 直接执行。
5. **地址不是对象身份。** 所有 UObject 操作使用带 `session_id + index + serial` 的句柄，并在执行前重新验证。
6. **任何网络、JSON、文件写入和阻塞锁都不得出现在游戏线程 Hook 热路径。**

建议按“先止血、再换边界、后修功能”的顺序实施，而不是一次性重写所有页面或先做 UI 整理。

## 2. 审计基线

### 2.1 已执行检查

| 检查 | 当前结果 | 说明 |
|---|---|---|
| Core `Release|x64` MSBuild | 通过 | 只能证明可编译，不能证明注入、卸载和 UE 行为正确 |
| `npm run build` | 通过 | TypeScript/Vite 构建通过 |
| `cargo check` | 通过 | Tauri Rust Host 可检查通过 |
| `npm run lint` | 失败 | 当前为 17 errors / 16 warnings；其中 2 个 error 来自未被 ESLint 排除的 `src-tauri/target` 生成文件，源码自身仍有 15 个 error |
| 自动化测试 | 不存在 | 未发现 C++、Rust、TS 单元测试、集成测试或 E2E |
| CI | 不存在 | 未发现 GitHub Actions 或其他 CI 配置 |
| 目标进程验证 | 未执行 | 当前没有注入目标、实时 API 流量或干净卸载证据 |

### 2.2 问题等级

- **P0**：可能造成目标进程崩溃、内存破坏、卸载后执行代码，或确定生成无效产物；重构前必须止血。
- **P1**：核心功能错误、严重竞态、错误成功状态、不可控阻塞或安全边界缺失；IPC 切换前必须解决。
- **P2**：性能、契约、可维护性和用户可见一致性问题；领域模块重构时解决。
- **P3**：文档、编码、命名和工程卫生问题；不能阻塞安全修复，但发布前必须清理。

## 3. 已存在的 BUG 与架构缺陷

状态说明：

- **已复现**：通过本地命令稳定复现。
- **静态确认**：从当前可达代码可确定其行为，无需猜测。
- **待运行验证**：代码存在高风险或契约疑点，但需要目标游戏和具体 UE 版本确认实际结果。

### 3.1 P0 阻断问题

| ID | 状态 | 问题与证据 | 影响 | 处置阶段 |
|---|---|---|---|---|
| LIFE-001 | 静态确认 | `Dumper/Server/HttpServer.cpp:867-876` 为每个客户端创建 detached 线程，`Stop()` 只等待约 3 秒；随后 `g_Server.reset()` 和 `FreeLibraryAndExitThread` | Worker 仍可访问已释放 `Impl` 或已卸载 DLL 代码，形成 UAF/崩溃 | R1 |
| GT-001 | 静态确认 | `Dumper/API/GameThreadQueue.h` 的单槽任务保存调用方 `paramBuf` 裸指针；HTTP 线程超时返回后，游戏线程仍可能继续使用该缓冲区 | ProcessEvent 参数 UAF、目标进程内存破坏 | R1 |
| HOOK-001 | 静态确认 | `Dumper/API/HookApi.cpp:419-447` 即使部分 VTable 恢复失败也会清空原函数并继续卸载；同时没有 in-flight callback barrier | 残留 VTable 仍指向已卸载代码，或 Hook 调用不到原 ProcessEvent | R1 |
| INJ-001 | 静态确认 | `frontend/src-tauri/src/lib.rs:436-452` 超时后无条件 `VirtualFreeEx`，但远程线程可能仍在读取 DLL 路径 | 目标进程中的 LoadLibraryW 参数 UAF | R1 |
| OFF-001 | 静态确认 | `Dumper/Engine/Private/OffsetFinder/Offsets.cpp:539-569,691-705` 在关键偏移发现失败时按布局猜测默认值 | 错偏移会污染后续所有读写、Hook 和生成结果；与“不可 fallback”约束直接冲突 | R1/R2 |
| CALL-001 | 静态确认 | `Dumper/API/CallApi.cpp:171-183` 在请求 `use_game_thread=true` 但调度器未启用时，自动转为 HTTP Worker 线程直接调用 ProcessEvent | 线程亲和性错误，可触发随机崩溃或游戏状态破坏 | R1 |
| MEM-001 | 静态确认 | `Dumper/API/MemoryApi.cpp:164-243` 写内存时未检查 `VirtualProtect`，写过程无 SEH，恢复保护也不校验 | 访问冲突、错误保护恢复、目标进程崩溃 | R1 |
| DUMP-001 | 静态确认 | `Dumper/Settings.h:101` 声明 ZStandard；`MappingGenerator.cpp:419-454` 写入 Zstd 标记却复制未压缩 payload | 生成的 USMAP 头与载荷确定不一致，消费者会按 Zstd 解压无效数据 | R1 |
| WORLD-001 | 静态确认 | `Dumper/API/WorldApi.cpp:947-985,1360-1471` 在 HTTP Worker 直接改 `RelativeLocation/Rotation/Scale` 内存，不调用 UE 变换 API | 场景树、物理、网络复制和渲染状态可能与内存值不一致；写入本身也可能崩溃 | R1/R5 |
| STATE-001 | 静态确认 | `/status/reconnect` 只锁自身，其他 API、Hook、Dump 同时读取和使用会被重新初始化的全局 `Off::*`、Settings、ObjectArray | 重连期间出现撕裂状态、错误偏移和并发崩溃 | R1/R2 |

### 3.2 生命周期、初始化、偏移发现与平台层

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| LIFE-002 | P1/静态确认 | 没有 `Created -> Initializing -> Ready -> Stopping -> Stopped/Failed` 状态机；`/health` 永远返回 alive | 引入 `CoreRuntime` 单一所有者和真实 liveness/readiness/capability 状态 |
| LIFE-003 | P1/静态确认 | 路由注册会先安装 Hook，随后才 bind；bind 失败后 DLL 和 Hook 继续存活，旧 `runtime.ini` 也可能仍标记运行 | 先建立通信与状态发布，再启用能力；失败必须回滚并进入 Failed，不留 Hook |
| LIFE-004 | P2/静态确认 | `DllMain` 中 `CreateThread` 不检查返回值、不关闭线程句柄，也未调用 `DisableThreadLibraryCalls` | 最小化 DllMain；保存并管理启动线程句柄，显式处理 attach/detach |
| LIFE-005 | P2/静态确认 | F6 是全局卸载触发；默认 `AllocConsole` 会改变目标进程可见状态 | 卸载改为 Host 的受控 RPC；默认使用结构化日志，不创建控制台 |
| LIFE-006 | P1/静态确认 | `runtime.ini` 多次非原子写入、明文 Token、无进程启动时间/会话 ID、只支持单全局实例，异常退出会留下 `Running=1` | 运行会话改为 Pipe 实时发现；持久配置采用版本化原子写，运行态不落盘 |
| LIFE-007 | P1/静态确认 | `Main.cpp:284` 仅当 GameName 和 GameVersion 同时为空才查询 Kismet；版本探测成功会导致 GameName 永久为空；且查询发生在启动 Worker 而非游戏线程 | 独立解析两个字段；所有 ProcessEvent 通过已验证的 GameThreadExecutor |
| LIFE-008 | P1/静态确认 | `TryProbeEngineVersionFromImage` 直接扫描整个 `SizeOfImage`，没有逐节/逐页可读性约束或 SEH | 仅扫描 PE 中可读节并使用边界安全读取；失败返回明确诊断 |
| LIFE-009 | P2/静态确认 | Core 和 Rust 都在 LOCALAPPDATA 不可用时回退 temp/current dir；配置目录创建和 INI 写入错误大多被忽略 | 删除路径 fallback；无法建立配置目录就明确失败并携带 Win32 错误 |
| LIFE-010 | P2/静态确认 | API Token 使用 `mt19937_64`，且前后端仍保留 `uexplorer-dev` 默认值 | Core IPC 不使用共享 Token；外部 Gateway Token 使用 Windows CSPRNG 并存于 Host |
| OFF-002 | P1/静态确认 | 只有 Script offset 有较完整置信度；其他关键偏移即使未找到也继续启动 | 每个偏移记录来源、候选、验证项和置信度；关键偏移不满足门槛时对应能力不可用 |
| OFF-003 | P1/静态确认 | GEngine 多候选时直接使用第一个；GWorld 存在单/双解引用启发式 | 候选必须通过类型、对象数组、稳定性和交叉引用验证；歧义时明确失败 |
| PLATFORM-001 | P1/静态确认 | `PlatformWindows.cpp:768-787` 的 `CurrentSkips` 在每轮循环内重置，`SkipCount > 0` 无法按预期工作 | 增加 pattern scanner 单测并修正跳过计数生命周期 |
| PLATFORM-002 | P1/静态确认 | `GetModuleLdrTableEntry` 和 `GetAddressOfImportedFunctionFromAnyModule` 将 `InMemoryOrderLinks` 指针直接 cast 为结构起始地址；同文件另一实现却正确减了 `offsetof` | 统一安全的 LDR 遍历器，校验链表和模块边界 |
| PLATFORM-003 | P1/静态确认 | `GetModuleBase(name)` 对“模块未找到”结果直接解引用；`FindPattern` 对不存在的 `.text` 节也可解引用空指针 | 所有平台查找返回显式 Result，禁止空指针继续传播 |
| PLATFORM-004 | P2/静态确认 | 自定义 `IsBadReadPtr` 固定探测 8 字节，不接收真实长度，并存在检查后再访问的 TOCTOU | 建立带范围和访问类型的 `MemoryRegion` 校验；最终访问仍由 SEH 包裹 |
| PLATFORM-005 | P2/静态确认 | 32 位 `FindStringInRange` 分支对 `IsBadReadPtr` 的判断方向相反；当前 x64 配置未触发 | 若项目不支持 x86则从能力矩阵明确移除；若支持则修复并加入 x86 测试 |

### 3.3 游戏线程执行与 Hook 生命周期

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| GT-002 | P1/静态确认 | GameThreadQueue 只有一个全局槽，无法排队、限流、取消或公平调度 | 使用有界 MPSC 队列和拥有参数内存的 Task 对象 |
| GT-003 | P1/静态确认 | `Disable()` 不唤醒等待者，也不标记 queued/running task 的终止结果 | 停止时拒绝新任务、取消未执行任务、唤醒全部 waiter、等待 in-flight 清零 |
| GT-004 | P1/静态确认 | 原 ProcessEvent 调用没有 SEH/finally，异常会留下 `g_Processing=true` 且不通知等待者 | 用 RAII 完成状态迁移；异常转换为结构化错误并保证通知 |
| GT-005 | P1/静态确认 | 任务泵依赖 CDO VTable 的 PostRender；无渲染、最小化、加载阶段可能长期不触发 | 抽象 `IGameThreadPump`；当前 PostRender backend 必须验证真实线程和活性，不满足即禁用能力 |
| GT-006 | P1/静态确认 | 找不到全局 ProcessEvent 时仍用空 lambda `Enable`，表面上报告已启用但调用实际什么都不做 | 删除空回调；初始化失败必须使 `function_call` capability 为 unavailable |
| GT-007 | P1/静态确认 | 没有 deadline、queued/running/cancelled/expired 状态以及卸载 drain 协议 | IPC task 包含 request ID、单调时钟 deadline 和确定的终态 |
| HOOK-002 | P1/静态确认 | `RegisterAllRoutes -> InitHooks` 在启动时无条件 Hook 全局 ProcessEvent，即使用户没有任何监控项 | PostRender 仅在需要游戏线程泵时安装；ProcessEvent 监控 Hook 按首个订阅启用、最后一个订阅关闭 |
| HOOK-003 | P1/静态确认 | Hook 命中在游戏线程执行锁、字符串解析、JSON 构造和 SSE/WS 广播 | 热路径只写入预分配有界事件环；序列化和分发全部移到非游戏线程/Rust Host |
| HOOK-004 | P1/静态确认 | 只扫描初始化时已有 Class CDO VTable；后加载/动态 Class 的独立 VTable 不会被 patch | 明确支持范围；增加按模块/类加载事件更新或采用经验证的单一拦截点 |
| HOOK-005 | P1/静态确认 | Disabled hook 的函数地址仍留在 `g_MonitoredFunctions`；命中仍写日志和广播，只是 HookId 变为 0 | 监控快照只包含 enabled 项；切换状态原子替换快照 |
| HOOK-006 | P2/静态确认 | 可重复添加同一函数；删除不存在的 ID 也返回 `removed=true`；列表字段 `vtable_hook_installed` 误读 PostRender 状态 | 唯一约束、准确 404、分离 PE/PostRender 状态字段 |
| HOOK-007 | P2/静态确认 | CallerName 从未赋值；参数、返回值、条件过滤未实现，但设计文档宣称已完成 | 未实现前从 UI/文档移除；实现后用显式 capture policy 和大小上限 |
| HOOK-008 | P1/静态确认 | 无采样、速率限制、丢弃计数或慢消费者策略 | 每订阅有界队列；丢弃/合并策略和 `dropped_count` 必须外显 |

### 3.4 当前 HTTP/SSE/WebSocket 层

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| NET-001 | P1/架构缺陷 | 自制 HTTP、SSE、WS 解析器运行在注入 DLL 内 | 整层移到 Rust Host；Core 只保留 Named Pipe framing |
| NET-002 | P1/静态确认 | Token 查找使用大小写敏感的 `Headers.find("X-UExplorer-Token")`，违反 HTTP header 名大小写不敏感规则 | Rust Gateway 使用成熟 HTTP 库和标准 HeaderMap |
| NET-003 | P1/静态确认 | 无 recv/send timeout，慢请求可永久占用 detached worker；并发上限检查与 worker 计数递增存在竞态 | R1 临时加入可控 worker/timeout；R4 移除 DLL 网络层 |
| NET-004 | P1/静态确认 | 超大 Content-Length 被静默截到 10 MiB；非法/负数变 0；不拒绝 TE/CL 歧义、重复长度或未完成 header/body | 严格返回 400/411/413；最终由 Rust HTTP 库处理 |
| NET-005 | P1/静态确认 | 普通响应和部分错误响应只调用一次 `send`，未处理 partial send | R1 使用统一 SendAll；R4 删除实现 |
| NET-006 | P1/静态确认 | 任意 `/api/v1/events/*` 都被特殊分支接成 SSE，未注册路径不会 404；注册的 EventsApi handler 实际不可达 | 事件订阅改为显式 IPC/Tauri event；路由必须精确匹配 |
| NET-007 | P1/静态确认 | SSE heartbeat 与广播、WS handler 与广播可并发向同一 socket 写，缺少每连接串行化 | 每连接单 writer task；Core 不直接持有客户端 socket |
| NET-008 | P1/静态确认 | SSE/WS 全局 mutex 和 EventsApi 的 server mutex 在阻塞 send 期间持续持有 | EventHub 使用无阻塞入队；慢消费者独立断开，不阻塞生产者 |
| NET-009 | P2/静态确认 | WS 事件把 JSON data 再编码成字符串；前端得到双层 JSON | 定义单一事件 envelope，`data` 保持 JSON 值 |
| NET-010 | P1/静态确认 | WS 未强制 client mask，也不完整校验 fragmentation、RSV、control frame 和 UTF-8 | Rust Gateway 使用成熟 WS 实现；删除自制协议栈 |
| NET-011 | P1/静态确认 | WebSocket Token 放 query，CORS/Origin 为 `*`，强能力 API 的浏览器边界过宽 | 桌面 UI 不走 HTTP；外部 Gateway 默认禁 browser Origin，Token 不进 URL |
| NET-012 | P1/静态确认 | 固定端口失败会依次尝试 27015-27018 和随机端口，属于静默 fallback | 固定模式 bind 失败即失败；只有用户显式选择 auto 才请求系统端口 |
| NET-013 | P1/静态确认 | `Stop()` 与 AcceptLoop 都可关闭 ListenSocket；WSA cleanup 可能发生时 detached socket worker 仍在运行 | R1 单一 socket owner 和 join；R4 移除 |
| NET-014 | P2/静态确认 | 响应 envelope、timestamp 单位、错误码和 status phrase 不一致；无 request ID、协议版本、duration | 统一协议错误码、request/session ID 和稳定版本 |
| NET-015 | P2/静态确认 | SSE 事件无 sequence/replay cursor/schema/drop count；HookName 和 WatchId 过滤参数未使用 | Event envelope 增加 seq、kind、session、drop metadata；订阅过滤在 Host 完成 |

### 3.5 UObject、类型、内存与 World 领域

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| OBJ-001 | P1/静态确认 | 对象枚举、类遍历、属性读写都在 HTTP Worker 进行，期间 UE GC/ObjectArray 可变化 | 游戏线程分帧构建不可变 snapshot；Live 操作重新验证句柄并在游戏线程执行 |
| OBJ-002 | P1/静态确认 | API 身份大多只有 object index；GC 后 index 可复用到另一个对象 | `ObjectHandle(session,index,serial)`；每次执行前核对 serial/address/class fingerprint |
| OBJ-003 | P1/静态确认 | 多处裸解引用只包 C++ `catch(...)`；`/EHsc` 不捕获访问冲突；`IsBadReadPtr` 又存在 TOCTOU | 统一 SafeMemory 层，范围校验 + SEH；禁止 API 模块直接 reinterpret_cast 读写 |
| OBJ-004 | P1/静态确认 | FText unresolved、Map/Set/Delegate 未实现却返回 `value_state=ok` | 使用 `ok/empty/unavailable/unsupported/error` 明确状态，未实现不能伪装成功 |
| OBJ-005 | P1/待运行验证 | SoftObject 读取把前 8 字节解释成 weak index/serial，可能不符合具体 UE 的 FSoftObjectPtr/FSoftObjectPath 布局 | 按版本能力和反射元数据实现 codec，并用真实 fixture 验证 |
| OBJ-006 | P1/静态确认 | 数组只探测首地址，未验证整个元素范围；嵌套 Struct 未完整检查 offset/size；outer/super 链缺少统一 cycle guard | 所有容器使用快照、大小上限、溢出检查和深度限制 |
| OBJ-007 | P2/静态确认 | 属性列表/组件列表只遍历当前 class 的 GetProperties，单属性查询却遍历父类，结果不一致 | 定义 direct/inherited 查询语义并在协议中显式选择 |
| OBJ-008 | P1/静态确认 | 多个 `stoi/stoull` 接受尾随垃圾或抛出 500；负 limit 未统一限制；过滤后的 `/objects.total` 仍是原始对象总数 | 统一 schema validation，非法输入固定 400，分页 total/matched 语义一致 |
| OBJ-009 | P2/静态确认 | 搜索、by-address、by-path、class/enum 查找反复 O(N) 扫描；分页无 snapshot generation，结果会漂移 | Rust 缓存 snapshot 和索引；请求携带 generation/cursor |
| OBJ-010 | P2/静态确认 | Class/Enum 以短名称解析，包之间重名时选择第一个；部分 path segment 未统一 URL decode | 使用 full object path/handle 作为主键，短名只做搜索结果而非执行身份 |
| OBJ-011 | P1/静态确认 | package contents 无分页和上限，可能产生巨型响应 | 所有集合端点强制 cursor/limit 和响应大小上限 |
| OBJ-012 | P1/静态确认 | 属性写直接改 UObject 内存，不走游戏线程、无变更通知/复制、数值范围校验和可靠 protection restore | 区分 Engine write 与 Raw write；前者走游戏线程及 UE API，后者显式标记且事务化 |
| CLASS-001 | P1/静态确认 | Instance API 通过 outer 名称等于 class 名来“跳过 CDO”，可能误判；hierarchy 无 cycle/depth guard | 通过 EObjectFlags 或与真实 CDO 句柄比较；层级遍历统一 cycle guard |
| MEM-002 | P1/静态确认 | 地址解析可接受部分字符串；pointer `deref + offset` 无溢出检查；offset 数组无独立数量上限 | 采用十六进制字符串/`u64` schema，严格完整解析和 checked_add |
| MEM-003 | P1/静态确认 | pointer-chain 中途失败仍返回 HTTP 200 的 success envelope，只把错误放 data 内 | 失败使用稳定错误码和失败 envelope，保留已完成 steps 作为 details |
| MEM-004 | P2/静态确认 | typed write 类型不对称、数值可静默截断；改可执行内存后不 FlushInstructionCache | 严格 typed codec、范围检查；代码写入单独能力并刷新指令缓存 |
| WORLD-002 | P1/静态确认 | level/component/Vec3 多处直接解引用，部分仅用 C++ catch；容器可在遍历中变化 | World snapshot 在游戏线程分帧构建，禁止 route 中直接遍历活容器 |
| WORLD-003 | P2/静态确认 | actor list 收集到当前页后 `break`，返回的 `matched` 只是扫描到该页时的计数，不是匹配总数 | 明确定义 cursor 分页；若不计算总数则不返回伪 total |
| WORLD-004 | P1/静态确认 | shortcuts 用 class 名 substring 的第一个全局对象，未限定当前 World，也可能选 CDO/Archetype | 从当前 World/GameInstance/LocalPlayer 链解析并验证对象句柄 |
| WORLD-005 | P2/静态确认 | level 收集逻辑在 helper 和 route 重复，且 debug 大对象随普通响应返回 | 单一 WorldService；debug 进入显式 diagnostics command |
| WORLD-006 | P1/待运行验证 | Vec3 根据全局 LWC 或属性大小猜 float/double；Rotation 仍用 x/y/z，可能掩盖 FRotator 语义 | 依据 Struct 类型路径选择 FVector/FRotator codec，并做 UE4/UE5 LWC fixture 测试 |

### 3.6 Function Call、Watch 与 Blueprint

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| CALL-002 | P1/静态确认 | `paramSize <= 0` 时猜 256 字节；未验证 `offset + size <= buffer` | 无合法 param struct size 就拒绝；每个字段严格边界检查 |
| CALL-003 | P1/静态确认 | 参数仅支持少量基础数值；FString/FName/UObject/Struct/Array 生命周期、构造和析构未处理 | 建立版本化 PropertyCodec 与 ParamFrame RAII，只开放有测试的类型 |
| CALL-004 | P1/静态确认 | 缺少参数被静默保持 0；InOut 因 OutParm 被完全跳过；错误归类为“unsupported” | 根据 CPF flags 生成必填 schema，区分 input/out/inout/return |
| CALL-005 | P1/静态确认 | API 暴露 `use_game_thread=false`，允许任意客户端主动绕过线程亲和性 | 从公共协议删除；UE 行为调用永远由 Core 决定并验证执行线程 |
| CALL-006 | P1/静态确认 | batch 数量无上限，每项可等待 10 秒，单请求可长期占用并生成巨型结果 | 有界 batch、总体 deadline、逐项状态和可取消 job |
| CALL-007 | P1/静态确认 | function/class 以短名解析，目标只用 index；执行前未重新验证 function owner 和对象 serial | FunctionHandle + ObjectHandle，并在游戏线程执行点再次验证 |
| WATCH-001 | P1/静态确认 | 没有独立采样线程/游戏线程任务；只有 GET `/watch/list` 才读取和检测变化，`interval_ms` 完全未使用 | 建立 WatchScheduler，按到期时间在受控预算内采样 |
| WATCH-002 | P1/静态确认 | 前端 SSE 正常时停止 300ms polling；而服务端只有 polling 才会产生事件，因此 Watch 会在首次刷新后冻结 | R1 关闭错误“实时”宣称；R5 完成真正 scheduler 后再启用事件 |
| WATCH-003 | P1/静态确认 | 持有全局 watch mutex 时做 UE 读取和同步广播，慢客户端会阻塞所有 Watch 操作 | 锁内只复制/更新状态，读取和事件入队分离 |
| WATCH-004 | P2/静态确认 | 无 max watches、session/object serial；ID 重载后复用；FName 读取仍靠 C++ catch；history 仅按条数不按字节限制 | session-scoped ID、有界 watch/history、统一 codec 和 drop/coalesce 统计 |
| BP-001 | P1/静态确认 | 实现更接近 best-effort bytecode pretty printer；未知 opcode 只消费一个字节并继续，仍返回 success/pseudocode | 输出 parse status、unknown count、first error offset、coverage；不完整时不能标记完整反编译 |
| BP-002 | P1/静态确认 | opcode 兼容只特判字符串含 `4.26`；FName 大小存在 heuristic fallback；pointer 固定按 64 位读 | 建立明确 UE bytecode profile；profile 不匹配就 capability unavailable |
| BP-003 | P2/静态确认 | Reader 越界时返回 0 而不记录错误，调用方无法区分真实 0 与截断 | Reader 返回 Result 并保持 sticky error，解析结束验证完整消费 |

### 3.7 Dump、注入与进程管理

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| DUMP-002 | P1/静态确认 | SDKDump 页面发送 include/exclude/blueprint/padding/assert 等选项，但四个 handler 不解析 `req.Body` | 定义 per-format schema；不支持的 option 拒绝，支持的 option 必须可在产物中验证 |
| DUMP-003 | P1/静态确认 | `joinable()` 被当成“线程已完成”；启动第二个 job 时会同步 join 仍在运行的前一个 job | 单一 DumpExecutor/job queue；完成通过 future/state，不在请求线程 join |
| DUMP-004 | P1/静态确认 | Generator/Settings/manager 使用共享全局状态，却允许创建多个线程；当前偶然阻塞不等于正确串行化 | 明确一次仅运行一个生成任务，后续任务排队或返回 Busy |
| DUMP-005 | P1/静态确认 | 无取消、真实进度和阶段；卸载会无限等待生成线程 | cooperative cancellation、阶段进度、deadline；不能安全停止时拒绝卸载而非强制卸载 |
| DUMP-006 | P2/静态确认 | job ID 在 DLL 重载后复用；map 输出无序；prune 每次只删一个；无 session、artifact hash/manifest | JobManager 位于 Host，使用 UUID；记录参数、阶段、产物清单、大小、hash 和验证结果 |
| DUMP-007 | P1/静态确认 | MappingGenerator 等未完整检查文件 open/write/malloc/压缩返回值；Generate 返回即标 completed | 所有 I/O/压缩返回 Result；完成态必须通过格式消费者或结构验证 |
| INJ-002 | P1/静态确认 | 等到 remote thread signaled 后不调用 `GetExitCodeThread`；LoadLibraryW 返回 NULL 仍报告 success | 成功条件必须是 wait 成功且 exit code/HMODULE 非 0 |
| INJ-003 | P1/静态确认 | 不校验 Host、目标进程、DLL 的 bitness；不 canonicalize/验证实际 DLL 文件；不检查是否已加载 | 注入前做 PE machine、绝对路径、文件 identity、目标 PID/start-time、模块列表检查 |
| INJ-004 | P1/静态确认 | 使用 PROCESS_ALL_ACCESS；创建 suspended thread 再 Resume，Resume 结果被忽略；多处错误丢失 Win32 code | 只请求所需权限，直接创建运行线程或完整校验 Resume，错误使用稳定 code + Win32 detail |
| INJ-005 | P1/静态确认 | 前端在注入返回 success 后即关闭弹窗；未要求 Core IPC handshake/readiness 成功 | 注入、IPC connected、Core ready 是三个独立状态；只有 ready 才宣告连接成功 |
| INJ-006 | P2/静态确认 | `inject_dll.ps1` 未被 Rust 路径使用，成功后泄漏远程参数内存，也不校验 wait/exit code | 标记 legacy 并从发布/文档路径移除；保留原件归档，不作为 fallback |
| PROC-001 | P2/静态确认 | Rust 和 React 各维护一套 UE 进程启发式过滤，可能互相矛盾且有误判/漏判 | Host 返回统一候选和证据；UI允许显式选择，不进行第二次隐藏过滤 |

### 3.8 React/Tauri、配置与产品行为

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| UI-001 | P1/架构缺陷 | `frontend/src/api/index.ts` 同时承载 30KB 类型、REST、SSE、WS、配置恢复和注入调用 | 拆为 generated contracts、Tauri transport、domain services、session store |
| UI-002 | P1/静态确认 | 普通 fetch 无 timeout/AbortSignal；请求可永久挂起；JSON parse 错误也会被当成 Network error | Host 统一 deadline；UI query 可取消并区分 transport/protocol/domain error |
| UI-003 | P1/静态确认 | 401 或网络错误会读取全局 `runtime.ini` 并自动换端点后重试，可能切到另一个 PID/session | 删除 endpoint recovery；所有请求必须显式绑定 session ID |
| UI-004 | P1/静态确认 | API constructor 异步恢复端点，但 Dashboard/Memory 同时立即创建 WS；连接不会随 endpoint 改变重建 | Session ready 后再建立 Tauri event subscription，由 SessionManager 管理重连 |
| UI-005 | P2/静态确认 | 自制 SSE parser 不处理 id/retry/EOF final event，buffer 无上限；页面另有硬编码 polling fallback | 桌面路径删除 SSE；外部 Gateway 由 Rust 库实现，UI 不做 transport fallback |
| UI-006 | P1/静态确认 | WS console 后端只回 `Console bridge connected`；前端一旦 WS connected 就绕过本地真实命令分支，因此界面宣称的 get/set/call 等命令不执行 | R1 隐藏/禁用 Console；R5 实现单一命令解析服务后再开放 |
| UI-007 | P1/静态确认 | Memory 页面使用 JS `Number/parseInt` 计算 x64 地址，超过 `2^53-1` 会丢精度并显示/写入错误地址 | 地址全程用规范化十六进制字符串或 BigInt，禁止转 Number |
| UI-008 | P1/静态确认 | Functions 的 Native/Blueprint 分类按函数名猜；“class filter”实际传成 package；实例调用默认 targetIndex 却取 UClass 对象 index | 使用后端元数据 flags/owner handle；实例必须从实际 instance 列表选择 |
| UI-009 | P1/静态确认 | 搜索 useEffect 每次按键直接发 O(N) 查询，无 debounce/cancel/request generation；旧响应可覆盖新状态 | query key + debounce + cancellation + generation guard；主要搜索查询 Host snapshot 索引 |
| UI-010 | P1/静态确认 | 每次 Hook hit 触发 hooks 和 log 两次完整请求；高频函数形成事件放大 | 事件直接携带增量数据，UI 本地归并；必要时低频 reconciliation |
| UI-011 | P1/静态确认 | Settings 中 Auto Reconnect、DLL Proxy、Default Dump Format、Output Directory、Light Theme、地址/数字格式、手工 offset 大多未接入实际行为 | 未实现项从可交互 UI 移除；重新加入时必须有端到端测试 |
| UI-012 | P1/静态确认 | Token/port 文案称立即生效，但已注入 DLL 不重载配置；改 Token 后当前连接反而 401，再被旧 runtime 状态覆盖 | Core IPC 无端口/Token；Gateway 配置由 Host 事务式应用并返回实际监听状态 |
| UI-013 | P1/静态确认 | SDKDump 进度固定显示 60%，选项未生效，detail 不随 job 自动刷新 | 只显示后端真实 phase/progress；未知进度使用明确 indeterminate 状态 |
| UI-014 | P2/静态确认 | Console logs 可无限增长；Watch history 首次读取后缓存不失效；多个轮询缺少 in-flight/卸载统一管理 | 有界 store、事件驱动更新和统一 query lifecycle |
| UI-015 | P1/已复现 | ESLint 当前 17 errors / 16 warnings；包括 hooks dependency、access-before-declare、`any`、unused、set-state-in-effect、Fast Refresh，以及错误扫描 `src-tauri/target` | 先修 ignore 边界，再清零源码 lint；CI 将 lint 设为必过 |
| UI-016 | P2/静态确认 | `scanUEProcesses` 捕获错误后返回空数组，UI 无法区分“没有进程”和“扫描失败” | 不吞错误；展示稳定错误码和可操作 detail |
| SEC-001 | P1/静态确认 | Tauri CSP 为 null；Token 和绝对开发 DLL 路径进入 localStorage；WS Token 进入 URL | 设置严格 CSP；桌面 UI 不保存 IPC secret；DLL 路径由 bundle/resource resolver 取得 |
| UI-017 | P2/静态确认 | 多个页面重复属性编辑、值解析和列表状态逻辑，类型中仍存在 `any` | 提取共享 domain model/component，仅保留一种 PropertyEditor/Address 类型 |
| UI-018 | P2/静态确认 | Dashboard 把永真 health/status 当作连接和引擎就绪，WS 也只连接一次 | UI 明确显示 Host、IPC、Core、Engine、Capability 五层状态 |

### 3.9 工程与文档缺陷

| ID | 级别/状态 | 问题与证据 | 目标修复 |
|---|---|---|---|
| ENG-001 | P1/已复现 | 仓库没有自动化测试和 CI | 先建立 harness 和 protocol tests，再允许大规模移动代码 |
| ENG-002 | P2/静态确认 | 现有验证主要是“能编译”；没有注入、handshake、断连、pending task 卸载、格式消费者验证 | 建立分层测试矩阵和可重复 fixture |
| ENG-003 | P2/静态确认 | 无根 README；`frontend/README.md` 仍是 Vite 模板 | 增加准确的项目说明、构建、运行、边界和诊断文档 |
| ENG-004 | P2/静态确认 | `DESIGN.md`/`PROJECT_MAP.md` 宣称 Watch、Console、WebSocket、审计修复等“已完成”，与当前行为矛盾 | 进度改为证据驱动：Implemented / Verified / Partial / Disabled |
| ENG-005 | P3/静态确认 | 多个 C++ 注释和 DESIGN 内容存在 mojibake/编码混乱 | 统一 UTF-8，增加 editorconfig 和编码检查 |
| ENG-006 | P2/静态确认 | `WorldApi.cpp`、`Functions.tsx`、`api/index.ts` 等超大文件混合多职责；Core 广泛使用 namespace global | 按 runtime/service/codec/transport 分层并引入显式 owner/context |
| ENG-007 | P2/静态确认 | ESLint 未排除 Rust target 生成资产；C++ 仅 `/W3`，无警告即错误或静态分析门槛 | 修正工具作用域；逐阶段提升 `/W4`、clang-tidy/MSVC analyze，先建立可控 baseline |
| ENG-008 | P1/静态确认 | 没有机器可读协议 schema、版本兼容规则或 contract tests | `protocol/` 作为唯一契约源，major 不兼容立即失败，minor 仅通过 capability 协商 |
| ENG-009 | P2/静态确认 | 无结构化 request/session 日志、队列深度、drop count、crash/minidump 关联 | 增加诊断 envelope、Host 日志和可选 minidump |

## 4. 目标架构详细设计

### 4.1 Core DLL 职责边界

Core 只负责必须在目标进程内完成的工作：

- 初始化和验证 UE runtime、偏移与能力。
- 创建对象/类型/World 的一致性 snapshot。
- 在已验证的游戏线程执行 UObject 写入和 ProcessEvent。
- 采集 Hook/Watch 原始事件并写入有界队列。
- 执行依赖进程内 UE 数据的 Generator。
- 通过一个可停止、可 join 的 Named Pipe transport 接收 RPC、返回结果。

Core 明确不负责：

- HTTP、CORS、WebSocket、SSE、浏览器认证。
- UI 配置持久化、外部 API Token、跨会话 job 展示。
- 慢客户端 fan-out、重试、缓存和前端 query 状态。
- 在 Hook 回调中做 JSON、磁盘 I/O 或 socket I/O。

### 4.2 Rust Host 职责

- `SessionManager`：按目标 PID 管理 Pipe、session ID、capabilities、状态和重连。
- `InjectionService`：架构/路径/PID 校验、最小权限注入、remote exit code、IPC readiness。
- `CoreRpcClient`：framing、deadline、取消、request map、事件读取、断线处理。
- `DomainService`：对象、函数、内存、World、Watch、Hook、Dump 的唯一业务入口。
- `SnapshotCache`：缓存 Core snapshot，建立 name/path/class/package 索引和稳定分页。
- `EventHub`：把 Core 事件 fan-out 到 Tauri event 和可选外部 WebSocket。
- `JobManager`：保存 dump job 参数、进度、状态和 artifact manifest。
- `Gateway`：显式启用的 localhost HTTP/WS 适配器；不允许复制领域逻辑。
- `SettingsStore`：版本化、原子写入；外部 Token 使用 Windows CSPRNG/受保护存储。

### 4.3 React 前端职责

- 只调用 Tauri command，并订阅 Tauri event。
- 通过 session store 绑定当前目标，不猜端口、不读取 runtime.ini。
- 使用生成的 TypeScript contract 和运行时 schema validation。
- query 有明确的 key、deadline、cancel 和 stale-response guard。
- UI 只显示 capability 已支持的控制项；未实现能力显示明确 unavailable，不提供替代路径。

### 4.4 CoreRuntime 状态机

```text
Created
  -> Initializing
      -> Ready
      -> Failed
Ready
  -> Stopping
      -> Stopped
Failed
  -> Stopping -> Stopped
```

规则：

1. `Ready` 必须同时满足：关键偏移验证、Pipe listener 就绪、GameThreadPump（如所需）验证完成。
2. optional capability 可独立 unavailable，但必须附带稳定 reason code。
3. `Stopping` 后立即拒绝新请求；取消 queued task；等待 I/O、Hook callback、game task 和 dump task达到安全点。
4. 任一 Hook 未恢复或 callback 未 drain 时，**拒绝 FreeLibrary**，返回 shutdown failure；不得强制卸载。
5. `/health` 的概念拆为 `liveness`、`readiness` 和 capability map，不能再用一个永真布尔值。

### 4.5 IPC v1

Pipe 命名建议：`\\.\pipe\UExplorer\v1\<pid>`。Core 创建 server，Rust Host 连接后使用 Windows API 核对 server PID 等于所选目标 PID；Pipe ACL 仅允许当前用户和 SYSTEM，禁止远程 pipe client。

帧采用固定小端 header + UTF-8 JSON payload：

| 字段 | 类型 | 说明 |
|---|---|---|
| magic | 4 bytes | `UEXP` |
| major/minor | `u16/u16` | major 不同立即断开；不做协议 fallback |
| kind/flags | `u16/u16` | Hello/Request/Response/Event/Cancel/Ping/Pong/Shutdown |
| payload_len | `u32` | v1 上限 8 MiB，读取 payload 前先校验 |
| request_id | `u64` | 请求/响应/取消关联；Event 为 0 |

握手：

1. Host 发送 `Hello {host_version, protocol, target_pid}`。
2. Core 返回 `Welcome {core_version, protocol, session_id, target_pid, engine_profile, capabilities, limits}`。
3. PID、major、session 任一不匹配立即失败；不尝试 HTTP、其他 pipe 或旧协议。

统一响应：

```json
{
  "ok": false,
  "request_id": 42,
  "session_id": "...",
  "error": {
    "code": "GAME_THREAD_UNAVAILABLE",
    "message": "PostRender pump is not active",
    "details": {}
  },
  "data": null,
  "timing": {"queued_us": 0, "execute_us": 0}
}
```

必须支持：

- partial read/write 和多帧粘连。
- 每请求 deadline；Cancel 只能取消 queued work，running work 的不可取消状态必须明确返回。
- 请求队列满返回 `BUSY`，不能静默丢请求。
- Event 带 `seq/kind/timestamp/session_id/dropped_before/data`。
- v1 compression 只允许 `none`。以后若新增 Zstd，必须作为新 capability 明确协商，不能在失败时发送未压缩数据冒充 Zstd。

### 4.6 队列与背压

初始硬上限在 benchmark 后可调整，但必须是协议可见配置：

- IPC frame：8 MiB。
- pending RPC：每 session 256。
- GameThread task queue：128；每帧默认最多消耗 1 ms，剩余任务延后。
- Hook raw event ring：8192；满时丢最旧事件并增加 drop counter，绝不阻塞游戏线程。
- Watch：按 watch ID 合并为最新值，保留 change sequence；不能靠无限事件堆积。
- UI/WS subscriber：每订阅 1024；慢订阅单独断开并报告原因。
- Dump：同时只运行 1 个；其余显式 queued，达到上限返回 Busy。

### 4.7 Snapshot、对象身份与线程规则

- `EngineSnapshot` 有 `generation`，包含已经复制出的对象 handle、名称、路径、class/package、必要元数据；Host 在 snapshot 上查询和分页。
- 构建 snapshot 采用游戏线程分帧预算，避免一次 O(N) 阻塞一帧；完成前不发布半成品。
- Live property read/write、ProcessEvent、World mutation 在游戏线程执行，并在执行点再次验证 object serial。
- 原始 memory read 可在专用 Worker 通过 SafeMemory 执行；UObject 语义读取不能因为“只是读”就绕过 GC 一致性。
- Actor transform 使用经反射验证的 UE setter/ProcessEvent；不可用就返回 capability error，不改字段冒充成功。

### 4.8 外部 HTTP Gateway

- 默认不作为桌面 UI 的 transport。
- 是否启用、固定端口还是 auto port 由用户显式选择；固定端口失败即失败。
- Gateway 复用 `DomainService`，因此 Tauri 和 curl 的行为、错误码、权限完全一致。
- 默认拒绝带 browser Origin 的请求；若未来开放浏览器客户端，使用明确 allowlist。
- Token 不写 URL、不存前端 localStorage；支持轮换和会话撤销。
- 对外 API 从 `/api/v2` 起步，避免把当前不一致的 `/api/v1` 假装成兼容实现。

## 5. 分阶段实施计划

粗略工期是单人净开发量，不含等待目标游戏、逆向新 UE 版本或发布审批；应以阶段验收门为准，而不是按日期强行结束。

### R0：证据固化与测试地基（3-5 人日）

任务：

1. 将本文 issue register 转成可跟踪清单，每项保留 ID、证据、owner、状态和验证记录。
2. 定义首批支持矩阵：至少一个 UE4.26 fixture、一个 UE4.27 fixture、一个 UE5/LWC fixture；没有 fixture 的版本不得宣称支持。
3. 新建 `tests/core-harness`：不注入真实游戏也能测试 framing、queue、shutdown、event backpressure。
4. 新建 `tests/fake-core`：Rust 可测试 Pipe partial frame、disconnect、protocol mismatch、deadline。
5. 修正 ESLint 扫描边界并清零源码 lint；保留生成目录，不执行删除。
6. 建立 Windows CI：前端 build/lint/test、cargo fmt/clippy/test、Core x64 build、protocol tests。
7. 保存当前 API 的 contract snapshot，标出哪些是历史兼容、哪些在 v2 删除。

验收门：

- 所有新测试在干净 checkout 可重复运行。
- CI 不依赖本机绝对路径。
- 当前每个 P0 都有最小复现或可执行的静态断言测试。
- 文档不再用“已完成”代替运行验证。

### R1：安全止血版本（5-8 人日）

任务：

1. 修复注入超时资源所有权；校验 `GetExitCodeThread`、bitness、PID identity、DLL canonical path。
2. 重写 GameThreadQueue task ownership、取消和 shutdown wakeup；删除直接 ProcessEvent 和 no-op callback fallback。
3. Hook 增加 in-flight barrier；恢复失败时禁止卸载；未订阅时不装 PE Hook。
4. 当前 HTTP server 在淘汰前改为 joinable workers、socket timeout、完整 SendAll；确保 Stop 后零 Worker。
5. Zstd 按 C 源正确编译并真实压缩；检查所有返回值，并用独立 USMAP consumer 验证。不能做到时该 job 明确失败，不写伪 USMAP。
6. 禁用或隐藏当前错误能力：WS Console、假实时 Watch、raw transform write、未生效 dump option、假 60% 进度。
7. reconnect 改为 stop-the-world 状态迁移；完成 CoreRuntime 前，运行中不允许重扫全局偏移。
8. critical offset 发现失败直接终止 Ready；先移除最危险的 guessed default。

验收门：

- P0 清零。
- 1000 次 connect/disconnect + shutdown harness 无遗留线程和 callback。
- game-thread task 超时后参数仍由 task 持有，直到任务确定终止。
- USMAP 能被选定消费者读取；输入/输出 size 与 compression method 一致。
- 不支持的功能在 API/UI 返回明确 unavailable，不存在替代执行路径。

### R2：CoreRuntime、能力模型与 EngineFacade（8-12 人日）

任务：

1. 引入 `CoreRuntime`、`EngineContext`、`CapabilityRegistry`、`ShutdownCoordinator`。
2. 把 global Off/Settings 的发布改为构建后一次性提交的 immutable context；读请求持有 context generation。
3. 为所有关键偏移建立验证报告和 capability dependency graph。
4. 实现有界 `GameThreadExecutor` 与经验证的 PostRender pump backend；记录 thread ID、last tick、queue depth。
5. Hook owner 使用 RAII token；安装、启用、停用、恢复均可测试。
6. 引入 SafeMemory，集中范围、SEH、checked arithmetic、protection restore。
7. 引入 `ObjectHandle`、`FunctionHandle` 及执行点 validation。
8. Core API handler 先改为领域 command，不再直接拼 HTTP response。

验收门：

- Core 状态和 capability 真实反映可用性。
- context 切换没有并发裸 global 读写。
- 没有 route/API 模块直接调用 VirtualProtect 或裸 ProcessEvent。
- shutdown 测试覆盖 queued/running Hook/Watch/Dump 场景。

### R3：Named Pipe RPC v1 与 Rust Session Host（8-12 人日）

任务：

1. 建立 `protocol/schema`，定义 header、message、error、capability 和 limits。
2. Core 实现一个可 join 的 overlapped Pipe server；Rust 实现 `CoreRpcClient`。
3. 完成 handshake、PID 验证、session ID、request map、deadline、cancel、ping/pong 和 disconnect。
4. 实现 event reader 与 Host EventHub；验证 seq/drop/backpressure。
5. 建立 FakeCore/RealCore contract tests 和 frame fuzz tests。
6. 实现 Rust SessionManager，多 PID 后端从一开始 session-scoped；UI 可先只激活一个 session。
7. 注入成功必须等待 Pipe connected + Core ready，错误分层返回。

验收门：

- 任意 1-byte 分片输入、粘连帧和中途断开都不会错帧或无限分配。
- major mismatch 立即失败且不尝试其他 transport。
- 慢 Host 不阻塞 Hook/game thread。
- Host 重启可显式重连同 PID 的 Pipe，旧 session handle 不会跨 DLL reload 使用。

### R4：通信边界原子切换（5-8 人日）

任务：

1. Rust DomainService 覆盖 status/object/type/memory/call/world/watch/hook/dump command。
2. React API 切到 Tauri invoke/event。
3. 如外部工具是发布需求，在 Rust 增加 `/api/v2` HTTP/WS Gateway。
4. 从 Core 工程构建中移除 HttpServer、EventsApi 和 HTTP route；源码按仓库“不删除”规则归档到 legacy，不保留运行入口。
5. 删除运行态 `runtime.ini` 依赖；旧 connection.ini 只做一次显式迁移，不做静默读取 fallback。
6. 清除前端 Token/port/WS/SSE 客户端路径。

迁移约束：

- 开发期间用独立构建配置测试旧路径或新路径，发布包中不能双栈并存。
- 切换提交必须同时包含 Core IPC、Rust Host、React transport 和 contract tests。
- 失败只能回退 Git 版本，不能在同一运行版本里自动切回 DLL HTTP。

验收门：

- 发布 DLL 不监听 TCP 端口，导入和代码路径中没有 HttpServer。
- React 全部功能不依赖 localhost fetch/EventSource/WebSocket。
- curl 需求若启用，只连接 Rust Host，且与 Tauri command 通过同一 contract test。

### R5：领域正确性重构（15-25 人日）

#### R5.1 Object/Type/Snapshot

- 分帧构建 snapshot，Host 建索引和 cursor pagination。
- 统一 full path、handle、继承字段语义、cycle guard。
- 完整区分 unsupported/unavailable/error；不再用占位字符串表示成功。
- 为 FString/FName/Text/Object/Weak/Soft/Struct/Array/Map/Set/Enum 建立有测试的 codec；未支持类型明确拒绝。

#### R5.2 Function Call

- 生成输入 schema，ParamFrame RAII 初始化/析构。
- 支持 input/out/inout/return；严格范围和类型验证。
- 删除直接线程选项；batch 改成有界可取消 job。
- 每个支持类型至少有一个 fixture function 和 round-trip test。

#### R5.3 World

- 从当前 World 链解析 actors/levels/shortcuts。
- Actor transform 通过 UE setter；不支持时失败。
- FVector/FRotator 和 LWC codec 基于 struct identity，不按 size 猜。

#### R5.4 Watch/Hook/Event

- WatchScheduler 使用 interval 和预算；对象失效产生 terminal event。
- Hook 热路径零网络、零 JSON、零磁盘；enabled snapshot 可原子替换。
- 事件包含 seq、capture policy、drop count；高频 UI 使用增量合并。

#### R5.5 Blueprint

- 将 bytecode profile 与 UE 版本能力绑定。
- Parser 记录 offset、错误和 coverage；未知 opcode 不再伪装完整反编译。
- UI 名称调整为“Bytecode Disassembly”或在达到真实控制流恢复后再称 Decompiler。

#### R5.6 Dump

- Host JobManager + Core 单执行器；真实阶段进度和取消点。
- per-format option schema、artifact manifest、hash 和 consumer validation。
- Zstd、file open/write/close 任一步失败都使 job Failed。

验收门：

- 每个对外 capability 都有至少一个目标 fixture 端到端测试。
- 不存在“字符串占位但状态 ok”“请求参数被忽略”“假进度”。
- GC churn 时旧 ObjectHandle 稳定返回 stale-handle，而不是操作新对象。

### R6：前端状态与交互重构（8-12 人日）

任务：

1. 目录拆分为 `contracts/transport/services/session/features`；删除巨型 `api/index.ts` 职责混合。
2. 引入统一 query cache（可使用 TanStack Query）和 event reducer；所有请求支持取消和 stale guard。
3. 地址类型改为 hex string/BigInt utility；禁止业务层 `Number(address)`。
4. Objects/Functions/Memory/World 使用 snapshot generation 和稳定 cursor。
5. Hook/Watch UI 直接消费增量事件，不因每次事件重新拉完整列表。
6. Settings 只展示已接入的设置；Gateway、注入、Dump、Display 分别由真实 service 应用。
7. 连接页显示 Host/IPC/Core/Engine/Capability 分层状态和明确失败原因。
8. 属性编辑器、值解析、错误展示和分页控件只保留一种实现。

验收门：

- lint/typecheck/unit/component tests 全绿。
- 快速输入搜索不会产生旧响应覆盖新结果。
- x64 地址在 `0xFFFFFFFFFFFFFFFF` 边界内可无损解析、显示和往返。
- UI 中没有不可用却可点击的“假功能”。

### R7：硬化、性能、文档与发布（8-12 人日）

任务：

1. Hook、snapshot、pipe、dump、断连和 shutdown stress/soak。
2. Frame/API fuzz；C++ harness 开启可用的 ASan/静态分析配置。
3. Tauri 设置严格 CSP，审计 command capability 和日志敏感字段。
4. 建立结构化日志、diagnostic bundle、queue/drop/timing metrics 和 minidump 关联。
5. 更新根 README、DESIGN、PROJECT_MAP、协议文档、支持矩阵和 troubleshooting。
6. 发布包从干净 checkout 构建，验证 DLL/Host/前端版本和 protocol major 一致。

验收门：

- 目标进程运行 soak 期间无未处理异常、无无限队列、无慢消费者造成游戏线程停顿。
- 干净卸载后所有 Hook 恢复、线程退出、Pipe 关闭，且没有 callback 进入卸载模块。
- 文档每个“已支持”都有对应测试或运行证据链接。

## 6. 验证矩阵

### 6.1 单元测试

**C++**

- Frame encode/decode、长度/溢出/partial I/O。
- GameThread task 状态机、timeout、cancel、shutdown drain。
- Hook enabled snapshot、ring overflow、drop count。
- ObjectHandle serial/session validation。
- SafeMemory checked range/protection restore。
- PropertyCodec/ParamFrame 各支持类型。
- Offset candidate validation 和“不可猜默认值”断言。
- USMAP header/compression round-trip。

**Rust**

- Injection permission、wrong arch、timeout、exit code 0、PID reuse、already loaded。
- Pipe handshake、major mismatch、request deadline、disconnect、reconnect。
- SessionManager 多 PID 隔离。
- DomainService 与 HTTP/Tauri adapter contract 一致。
- Settings 原子写、版本迁移和损坏文件错误。

**TypeScript/React**

- x64 address BigInt utility。
- runtime contract validation 和 error mapping。
- query cancellation、stale response、event reducer/drop metadata。
- capability gating 和 Settings 实际应用。

### 6.2 集成与目标运行测试

| 场景 | 必须验证的结果 |
|---|---|
| 注入成功 | remote exit code 非 0、Pipe handshake、Core Ready、版本匹配 |
| 注入失败/错架构 | 不释放仍被使用的远程内存，不宣告成功，不尝试替代注入法 |
| 重复注入 | 明确 AlreadyLoaded 或显式 reconnect，不加载第二份 Core |
| Pipe 断开 | Core 不阻塞游戏线程；Host 请求明确失败；重连仍为同 session |
| Host 退出 | Core 停止事件 fan-out并保持有界；策略明确，不转为 HTTP |
| 无 PostRender/最小化/加载 | game-thread capability 进入 unavailable/stalled，调用不转 Worker |
| GC churn | 旧 handle 返回 STALE_OBJECT；分页绑定 snapshot generation |
| 高频 Hook | 生产者不阻塞，drop count 可见，慢订阅被隔离 |
| Watch | interval 生效、对象失效有 terminal event、SSE/polling 不再参与 |
| Shutdown 有 pending RPC | queued 取消、running 达安全点、全部线程 join 后才卸载 |
| Shutdown 有 Dump | 正常取消或明确拒绝卸载，不能无限无状态等待 |
| USMAP | 被至少一个独立消费者成功载入，compression/size/hash 一致 |
| Actor transform | UE setter 生效，场景/物理可观察状态一致，不直接改字段 |

### 6.3 性能和资源验收

- 非命中 Hook 热路径：零堆分配、零阻塞 mutex、零序列化、零 I/O。
- 命中 Hook 热路径：只有 lookup + 有界 ring push；具体 p95/p99 阈值在 R0 基准后冻结并进入 CI。
- GameThreadExecutor 每帧有明确时间预算，超预算任务延后而不是拖长单帧。
- 所有 queue、history、log、frame 和 response 都有条数或字节上限。
- 搜索和分页查询 Host snapshot 索引，不因每次键入扫描完整 GObjects。
- shutdown 在无不可取消引擎调用时应在 5 秒内完成；否则返回明确阻塞原因并不卸载。

## 7. 目标目录建议

```text
protocol/
  schema/
  fixtures/

Dumper/
  Core/
    CoreRuntime.*
    CapabilityRegistry.*
    ShutdownCoordinator.*
  Transport/
    PipeServer.*
    FrameCodec.*
  Runtime/
    EngineContext.*
    GameThreadExecutor.*
    SnapshotService.*
    SafeMemory.*
  Services/
    ObjectService.*
    FunctionService.*
    WorldService.*
    WatchService.*
    HookService.*
    DumpService.*
  legacy/
    http_v1/              # 只归档，不进入构建

frontend/src-tauri/src/
  session/
  ipc/
  injection/
  services/
  gateway/
  settings/

frontend/src/
  contracts/
  transport/
  services/
  session/
  features/
  components/

tests/
  core-harness/
  fake-core/
  protocol/
  fixtures/
  e2e/
```

## 8. 顺序、依赖与版本切点

```text
R0 测试地基
  -> R1 P0 止血
      -> R2 CoreRuntime
          -> R3 IPC + Rust Session Host
              -> R4 原子切换
                  -> R5 领域正确性
                  -> R6 前端重构
                      -> R7 发布硬化
```

建议版本切点：

- **0.1.x Safety**：只修 P0/P1 止血，不新增表面功能。
- **0.2.0 IPC Cutover**：Core 移除 HTTP；Tauri/IPC 成为唯一桌面链路；外部 Gateway 如需要从 `/api/v2` 开始。
- **0.3.0 Verified Features**：只重新开放通过 fixture 验证的 Watch/Hook/Call/World/Blueprint/Dump 能力。

不得将 R5/R6 的大量功能重写与 R3/R4 的 transport 切换压成一个不可审查提交。每阶段使用小提交和协议 fixture 保持可回溯，但发布版本只能包含一条运行链路。

## 9. 风险与控制

| 风险 | 控制措施 |
|---|---|
| UE 版本差异导致 offset/bytecode/property layout 不稳定 | 明确 engine profile 和 fixture；capability 不满足即失败，不猜 |
| PostRender 并非所有场景稳定 | backend 可替换但不自动切换；运行时验证活性并暴露 stalled |
| Snapshot 分帧期间对象变化 | 构建代次、serial 验证、完成后原子发布 |
| Generator 依赖大量全局状态 | 在完成 context 化前强制单任务；产物独立目录和 manifest |
| IPC 重构引发前后端契约漂移 | schema + golden frames + FakeCore contract tests |
| 大改期间旧 HTTP 仍被误用 | 新旧路径用构建配置隔离；R4 发布前从 Core 工程移除旧入口 |
| 无真实游戏 fixture 阻塞验证 | Harness 先覆盖生命周期/协议；功能状态保持待验证，不提前勾选完成 |

## 10. Definition of Done

一次“完整重构”只有同时满足以下条件才算完成：

1. Core DLL 不监听 TCP，不含可达 HTTP/SSE/WS 运行路径。
2. 所有线程、Hook、Pipe、task 和 generator 都有明确 owner，可停止、可 join、可证明卸载安全。
3. UE 行为调用只能走经验证的游戏线程执行器；不可用时明确失败。
4. critical offset 不使用 guessed default；每个 capability 有依赖和验证报告。
5. UObject 操作使用 session/index/serial handle，GC 后不会误操作复用 index。
6. Hook 热路径无网络、JSON、磁盘和阻塞锁；所有队列有背压和 drop 指标。
7. Watch interval 真正生效；Console 命令真正执行；Dump option 和进度真实；USMAP 可被独立消费者读取。
8. React 只走 Tauri；外部 HTTP 若启用只存在于 Rust Host，并复用同一 DomainService。
9. 没有静默端口、transport、算法、offset、执行线程或命令 fallback。
10. Core build、Rust fmt/clippy/test、前端 lint/build/test、protocol/integration/E2E 和目标 fixture 验证均通过。
11. README、DESIGN、PROJECT_MAP 与当前行为一致，任何“已支持”都有对应验证证据。

## 11. 推荐的立即执行顺序

下一轮实现不要直接从“大规模目录移动”开始，按以下顺序提交：

1. R0-1：测试框架、CI、ESLint 边界、protocol ADR，不改运行行为。
2. R1-1：注入器资源所有权和成功判定。
3. R1-2：GameThread task ownership、取消、shutdown drain。
4. R1-3：Hook in-flight barrier、禁用错误能力。
5. R1-4：Zstd/USMAP 正确性和 consumer test。
6. R1-5：当前 server 可 join 退出，作为 IPC 切换前的临时安全修复。
7. R2：CoreRuntime/EngineContext/CapabilityRegistry。
8. R3-R4：IPC、Rust Host、React transport 原子切换。
9. R5-R7：按领域逐项恢复并验证功能。

这条顺序优先消除“会破坏目标进程”的风险，再消除错误架构边界，最后处理功能完整性和 UI 体验。
