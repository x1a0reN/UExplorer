# UExplorer 项目全量地图

## 一、项目概述

UExplorer 是一个面向 Unreal Engine 的 **SDK Dump + 实时游戏内省工具**，核心理念是将 Dumper-7 的离线 dump 能力升级为在线实时探索体验。

**目标架构：四层单向边界**

```
┌──────────────────────────────────────────────┐
│  React UI                                    │  ← 只使用 Tauri command/event
└───────────────┬──────────────────────────────┘
                │ Tauri invoke/event
┌───────────────┴──────────────────────────────┐
│  Rust Host                                   │  ← Session/注入/领域服务/EventHub
└───────────────┬──────────────────────────────┘
                │ Windows Named Pipe RPC v1
┌───────────────┴──────────────────────────────┐
│  Core DLL (C++, 注入游戏进程)                   │  ← 最小 UE 引擎内核
│  CoreRuntime / EngineFacade / GameThread      │
└───────────────┬──────────────────────────────┘
                │ 经验证的内存访问与 UE 调用
┌───────────────┴──────────────────────────────┐
│  Target UE Game Process (UE 4.11 ~ 5.x)     │
└──────────────────────────────────────────────┘
```

当前分支已完成 R4 原子通信切换并进入 R5.3。React -> Tauri `domain_request` -> Rust `DomainService` -> PID-scoped Named Pipe -> `CoreCommandService` 是唯一桌面主链路；release Core 不编译旧 HTTP/API，也不链接 WinSock。Object/Type 集合由 Host immutable SnapshotIndex 查询，详情由 `TypeCommandService` 读取 exact TypeSnapshot；`ObjectPropertyCommandService` 和 `FunctionCallCommandService` 分别执行稳定句柄属性读取与单目标函数调用。生产类型源现可冻结 flat descriptor、一层 `Array<flat>` descriptor，以及 exact `/Script/CoreUObject.Vector`/`Rotator` 的 float/double 三字段 descriptor；PropertyCodec 对 struct 先复制稳定整值再递归解码。只读 World 链路由 `WorldSnapshotCapture` 在 exact Object/Type generation 上预筛候选、在 PostRender 预算内解析并复核 GWorld/Level/Actor/ActorComponent、RootComponent、GameMode/GameState 及 `OwningGameInstance -> LocalPlayers[0] -> PlayerController -> Pawn` 关系，再由 Worker 发布不可变 snapshot；`WorldCommandService` 提供 inspect、Level/Actor/Component page、exact-handle Actor detail 与显式状态 shortcuts。WorldBrowser 通过现有 exact property command 展示 RootComponent 的三个存储 `Relative*` 字段；跨字段同帧快照、`bAbsolute*`、computed world transform 和 UE setter 尚未完成。函数复杂生命周期参数、batch job、Memory、Watch/Hook/Blueprint/Dump 仍未完成。真实 UE profile、调用/World round-trip、streaming、GC、Hook、目标规模和卸载证据仍以 `docs/SUPPORT_MATRIX.md` 为准，编译或 synthetic fixture 不改变支持声明。

---

## 二、核心文件树

```
UExplorer/
├── AGENTS.md                           # 当前 Agent 规则和验证契约
├── DESIGN.md                           # 当前事实、设计与历史记录
├── PROJECT_MAP.md                      # 本文档
├── UExplorerCore.slnx                  # VS2026 解决方案
│
├── protocol/                           ★ IPC v1 schema、Rust typed payload 与跨语言 fixture
│   ├── v1/                             #   JSON schema、协议说明与 golden data
│   └── rust/                           #   有界 framing + strict RPC/Handle/snapshot payload types
│
├── tests/                              ★ 分层契约与进程级运行夹具
│   ├── core-harness/                   #   真实 Core server、runtime、queue、shutdown harness
│   ├── injection-fixture/              #   x64/x86 target 与 Ready/slow/reject injectable DLL
│   ├── fake-core/                      #   Rust transport/session FakeCore
│   └── contracts/                      #   issue、架构与安全静态门禁
│
├── Dumper/                             ★ Core DLL（C++）
│   ├── UExplorerCore.vcxproj          # MSBuild 项目文件 (v145, C++latest)
│   ├── Main.cpp                       # DLL 入口 (DllMain → MainThread)
│   ├── Settings.h / .cpp             # 全局配置管理
│   ├── TmpUtils.h                    # 工具函数（Align, StrToLower, MakeValidFileName）
│   │
│   ├── Runtime/                       ★ Core 生命周期与安全边界
│   │   ├── CoreRuntime.h             #   状态机、request lease、readiness
│   │   ├── EngineContext*.h/.cpp     #   一次性发布的引擎 profile/offset report
│   │   ├── EngineFacade.h/.cpp       #   session/context/identity 的单一领域入口
│   │   ├── EngineNameCodec.h/.cpp    #   immutable layout + SafeMemory 的严格 FName 解码
│   │   ├── PropertyCodec.h/.cpp      #   显式状态、Windows x64 ScriptArray、struct 整值稳定快照与精确值树预算
│   │   ├── ParamFrame.h/.cpp         #   ProcessEvent owned frame、trivial lifetime 与精确字段边界
│   │   ├── ReflectionLayout.h/.cpp   #   U/FProperty 字段 witness、尺寸边界与分阶段原子 snapshot
│   │   ├── ReflectionLayoutCapture.* #   单条 evidence/预检预算、依赖复核、同线程发布与 drain owner
│   │   ├── ObjectSnapshotReflectionCandidateSource.* # snapshot + SafeMemory 的生产反射候选源
│   │   ├── TypeSnapshot.h/.cpp       #   完整类型覆盖、冻结 descriptor、exact Vector/Rotator resolver、继承/CDO 语义
│   │   ├── TypeSnapshotCapture.*     #   分帧 record owner、精确依赖封口、Worker publication/retirement
│   │   ├── TypeMetadataContext.h      #   类型采集所需 legacy discovery 结果的 immutable 子集
│   │   ├── ObjectSnapshotTypeCandidateSource.* # exact snapshot/layout + flat/单层 Array/Struct 引用身份的生产类型源
│   │   ├── WorldSnapshot.h/.cpp      #   current World/Level/Actor/Component generation、reference state 与严格 store
│   │   ├── WorldSnapshotCapture.*    #   Worker 预筛/封口/publication + PostRender 分帧 ownership/local-player 采集和复核
│   │   ├── EngineVersionProbe.h/.cpp #   只扫描已验证 PE 可读节的版本标记探测
│   │   ├── EngineSnapshot.h/.cpp     #   分段记录、validated publish、旧代 Worker retirement
│   │   ├── EngineSnapshotCapture.*   #   budgeted capture/validate/publish + failed-set retirement
│   │   ├── ObjectSnapshotIdentitySource.h # snapshot 所需的 typed slot identity 边界
│   │   ├── ObjectArrayIdentitySource.*    # 生产 FUObjectItem/Handle identity source
│   │   ├── ObjectArraySnapshotSource.*    # 生产 name/full/class/package/kind metadata source
│   │   ├── GameThreadFrameScheduler.* #   多 client 公平预算、time guard 与独立 drain
│   │   ├── GameThreadExecutor.h/.cpp #   有界 owned work、deadline、cancel、frame client、drain
│   │   ├── PostRenderHook.h/.cpp     #   生产 game-thread pump Hook owner 与 quiet drain
│   │   ├── ObjectHandle*.h/.cpp      #   serial-backed Object/FunctionHandle
│   │   ├── SafeMemory.h/.cpp         #   范围、SEH、保护恢复与代码写策略
│   │   └── VTableHook.h/.cpp         #   RAII patch owner
│   ├── Services/                      ★ transport-neutral Core 领域服务
│   │   ├── CoreCommandService.h/.cpp #   command 总入口、lease/capability/snapshot gate 与稳定错误 envelope
│   │   ├── TypeCommandService.h/.cpp #   worker-only exact-path immutable TypeSnapshot 详情/分页
│   │   ├── ObjectPropertyCommandService.* # exact Handle/generation 的游戏线程属性读取
│   │   ├── FunctionCallCommandService.* # exact Object/Function Handle 的单目标 ProcessEvent 调用
│   │   ├── WorldCommandService.*     #   worker-only World inspect/detail/shortcut 与 Actor-bound cursor query
│   │   └── CoreStatusDiagnostics.*   #   只读诊断源
│   ├── IPC/                           ★ Core Named Pipe RPC transport
│   │   ├── Protocol.h                #   24-byte framing/有界协商 decoder/limits
│   │   └── NamedPipeRpcServer.*      #   DACL/PID、overlapped I/O、request workers 与有界 Event writer
│   │
│   ├── Server/                        ★ legacy HTTP 源码（不在 release project，无运行入口）
│   │   ├── HttpServer.h              #   PIMPL 接口（HttpRequest/HttpResponse/RouteHandler/SSE/WS）
│   │   └── HttpServer.cpp            #   WinSock2 实现（路由匹配/Token/CORS/SSE/WebSocket）
│   │
│   ├── API/                           ★ legacy REST 适配层（13 模块，不在 release project）
│   │   ├── ApiCommon.h               #   JSON 响应信封 (MakeResponse/MakeError/ParseQuery)
│   │   ├── Router.h / .cpp           #   路由注册中心 (RegisterAllRoutes)
│   │   ├── GameThreadQueue.h         #   游戏线程调度队列 (Submit/ProcessQueue)
│   │   ├── WinMemApi.h               #   Windows 内存 API 前向声明
│   │   ├── StatusApi.h / .cpp        #   /status, /status/health, /status/engine, /status/reconnect
│   │   ├── ObjectsApi.h / .cpp       #   /objects/*, /packages/*
│   │   ├── ClassesApi.h / .cpp       #   /classes/*, /structs/*
│   │   ├── EnumsApi.h / .cpp         #   /enums/*
│   │   ├── DumpApi.h / .cpp          #   /dump/* (4 种格式, 后台任务)
│   │   ├── MemoryApi.h / .cpp        #   /memory/* (读/写/指针链)
│   │   ├── WorldApi.h / .cpp         #   /world/* (UWorld/Actor/Transform)
│   │   ├── CallApi.h / .cpp          #   /call/* (ProcessEvent 调用)
│   │   ├── BlueprintApi.h / .cpp     #   /blueprint/* (反编译/字节码)
│   │   ├── WatchApi.h / .cpp         #   /watch/* (属性监视 + SSE)
│   │   ├── HookApi.h / .cpp          #   /hooks/* (VTable 钩子 + SSE)
│   │   └── EventsApi.h / .cpp        #   /events/* (SSE 广播)
│   │
│   ├── Engine/                        ★ UE 引擎抽象层（源自 Dumper-7）
│   │   ├── Public/Unreal/
│   │   │   ├── Enums.h               #   EPropertyFlags, EFunctionFlags, EClassCastFlags...
│   │   │   ├── UnrealContainers.h    #   TArray, FString, TMap, TSet, TSparseArray
│   │   │   ├── UnrealTypes.h         #   FName, FFreableString, TImplementedInterface
│   │   │   ├── UnrealObjects.h       #   UEObject → UEField → UEStruct → UEClass → UEFunction + 20+ UEProperty 子类
│   │   │   ├── ObjectArray.h         #   GObjects 遍历 (ObjectArray, ObjectsIterator, AllFieldIterator)
│   │   │   └── NameArray.h           #   GNames 解析 (NameArray, FNameEntry)
│   │   ├── Public/OffsetFinder/
│   │   │   ├── OffsetDiscovery.h     #   GWorld/GEngine 候选证据与唯一性错误模型
│   │   │   ├── Offsets.h             #   Off::* 全部偏移定义 (30+ 命名空间)
│   │   │   └── OffsetFinder.h        #   自动偏移探测函数 (FindUObjectClassOffset 等)
│   │   ├── Public/Blueprint/
│   │   │   ├── EExprToken.h          #   Blueprint VM 操作码枚举 (80+ 操作码)
│   │   │   └── BlueprintDecompiler.h #   字节码反编译器 (Decompile/ParseExpression)
│   │   └── Private/                  #   对应 .cpp 实现
│   │       ├── Blueprint/BlueprintDecompiler.cpp
│   │       ├── OffsetFinder/OffsetDiscovery.cpp, OffsetFinder.cpp, Offsets.cpp
│   │       └── Unreal/NameArray.cpp, ObjectArray.cpp, UnrealObjects.cpp, UnrealTypes.cpp
│   │
│   ├── Generator/                     ★ SDK 生成器（源自 Dumper-7）
│   │   ├── Public/Generators/
│   │   │   ├── Generator.h           #   基础生成器 (InitEngineCore, InitInternal, Generate<T>)
│   │   │   ├── CppGenerator.h        #   C++ SDK 头文件生成 (GenerateStruct/Enum/Members/Function)
│   │   │   ├── MappingGenerator.h    #   USMAP 二进制映射 (EUsmapVersion)
│   │   │   ├── DumpspaceGenerator.h  #   Dumpspace JSON
│   │   │   ├── IDAMappingGenerator.h #   IDA Pro 脚本
│   │   │   └── EmbeddedIdaScript.h   #   内嵌 Python IDA 脚本
│   │   ├── Public/Managers/
│   │   │   ├── PackageManager.h      #   包组织 (PackageInfo, 依赖遍历, 循环检测)
│   │   │   ├── StructManager.h       #   结构体索引 (StructInfo, 唯一名检查)
│   │   │   ├── EnumManager.h         #   枚举索引 (EnumInfo, 碰撞处理)
│   │   │   ├── MemberManager.h       #   成员名称解析 (MemberIterator, FunctionIterator)
│   │   │   ├── CollisionManager.h    #   名称冲突处理 (NameInfo, ECollisionType)
│   │   │   └── DependencyManager.h   #   依赖排序 (拓扑排序, 回调遍历)
│   │   ├── Public/Wrappers/
│   │   │   ├── StructWrapper.h       #   UEStruct 封装 (GetName/Super/Members/Size)
│   │   │   ├── EnumWrapper.h         #   UEEnum 封装 (GetName/Members)
│   │   │   └── MemberWrappers.h      #   PropertyWrapper + FunctionWrapper + ParamCollection
│   │   ├── Public/
│   │   │   ├── HashStringTable.h     #   高性能字符串表 (Pearson 哈希, 桶迭代)
│   │   │   └── PredefinedMembers.h   #   预定义结构体覆盖 (FVector, FRotator 等)
│   │   └── Private/                  #   对应 .cpp 实现
│   │       ├── Generators/Generator.cpp, CppGenerator.cpp, MappingGenerator.cpp,
│   │       │              DumpspaceGenerator.cpp, IDAMappingGenerator.cpp
│   │       ├── Managers/PackageManager.cpp, StructManager.cpp, EnumManager.cpp,
│   │       │            MemberManager.cpp, CollisionManager.cpp, DependencyManager.cpp
│   │       ├── Wrappers/StructWrapper.cpp, EnumWrapper.cpp, MemberWrappers.cpp
│   │       └── HashStringTable.cpp
│   │
│   ├── Platform/                      ★ 平台抽象层
│   │   ├── Public/
│   │   │   ├── Platform.h            #   平台选择器 (PLATFORM_WINDOWS → PlatformWindows)
│   │   │   ├── Architecture.h        #   架构选择器 (→ Arch_x86)
│   │   │   ├── BytePattern.h         #   严格 pattern parser 与可测试 skip-aware scanner
│   │   │   └── PeImage.h             #   typed AMD64 PE image/section view 与错误模型
│   │   └── Private/
│   │       ├── PlatformWindows.h/.cpp #   x64-only 模式扫描、范围校验、安全 LDR/模块访问
│   │       ├── PeImage.cpp            #   SafeMemory 驱动的 PE header/section 边界验证
│   │       └── Arch_x86.h/.cpp       #   x86-64 指令解析 (RIP 相对跳转/调用, 函数边界)
│   │
│   └── Utils/                         ★ 工具库
│       ├── Utils.h                   #   GetImageBaseAndSize, FindUnrealExecFunctionByString
│       ├── Json/json.hpp             #   nlohmann/json (单头文件)
│       ├── Compression/zstd.h        #   ZStandard 压缩头 (zstd.c 已排除编译)
│       ├── Dumpspace/DSGen.h/.cpp    #   Dumpspace JSON 生成 (ClassHolder/EnumHolder/FunctionHolder)
│       └── Encoding/
│           ├── UnicodeNames.h        #   Unicode 字符分类 (XID_Start/XID_Continue)
│           └── UtfN.hpp              #   UTF-8/16/32 转换
│
└── frontend/                          ★ Tauri 桌面前端
    ├── package.json                  #   React + Vite + Tauri 依赖
    ├── vite.config.ts                #   Vite 构建配置
    ├── tsconfig.json                 #   TypeScript 配置
    ├── index.html                    #   HTML 入口
    ├── src/
    │   ├── main.tsx                  #   React 入口
    │   ├── App.tsx                   #   主布局 (侧栏导航 + 6 页路由)
    │   ├── index.css                 #   全局样式
    │   ├── api/index.ts              #   领域类型与公开 client export
    │   ├── api/client.ts             #   仅 Tauri invoke/Channel 的 typed desktop client
    │   ├── types/index.ts            #   TypeScript 类型定义 (~40 接口)
    │   ├── i18n/
    │   │   ├── index.ts              #   国际化入口
    │   │   └── translations.ts       #   翻译文本
    │   ├── components/
    │   │   └── ProcessSelector.tsx    #   UE 进程选择器 (扫描 + DLL 注入)
    │   └── pages/
    │       ├── Dashboard.tsx         #   仪表盘 (连接状态 + 统计卡片 + 快捷操作)
    │       ├── Objects.tsx           #   对象浏览器 (三面板: 层级/实例/检查器)
    │       ├── Functions.tsx         #   函数浏览器 (搜索/调用/Hook/反编译 四合一)
    │       ├── Memory.tsx            #   内存工具 (Hex 视图 + Console + Watch)
    │       ├── SDKDump.tsx           #   SDK 生成中心 (4 种格式 + 任务管理)
    │       ├── Settings.tsx          #   设置 (连接/DLL/显示/偏移覆盖)
    │       └── objects/              #   Objects 页子面板
    │           ├── HierarchyPane.tsx  #     类/结构体/枚举继承树
    │           ├── InstancePane.tsx   #     选中类的实例列表
    │           ├── InspectorPane.tsx  #     属性检查器 (读写)
    │           ├── TypeBrowser.tsx    #     类型浏览 (Class/Struct/Enum 切换)
    │           ├── InstanceBrowser.tsx#     实例浏览
    │           ├── WorldBrowser.tsx   #     世界浏览、exact RootComponent stored Relative* 读取
    │           └── shared.tsx        #     共享组件/工具
    └── src-tauri/
        ├── Cargo.toml                #   Rust 依赖
        ├── tauri.conf.json           #   Tauri 应用配置
        ├── build.rs                  #   Tauri 构建脚本
        ├── capabilities/default.json #   Tauri 权限配置
        └── src/
            ├── main.rs               #   Tauri 主入口
            ├── lib.rs                #   Tauri 命令；注入 -> Pipe -> Ready 门与 managed session/event state
            ├── ipc/
            │   ├── rpc_session.rs   #   严格握手/关联/deadline/cancel/event/shutdown 状态机
            │   └── named_pipe_client.rs # 真实 Win32 overlapped client、PID 核验、bounded queues 与 join
            ├── services/
            │   └── domain_service.rs #   显式 operation registry、session 绑定与 capability gate
            ├── session/
            │   ├── event_bridge.rs   #   EventHub -> 调用方 Tauri Channel 的有界 owned bridge
            │   ├── event_hub.rs      #   有界事件过滤、精确 replay、fan-out 与 drop 诊断
            │   ├── session_manager.rs#   多 PID Pipe/session 生命周期与 snapshot 拉取
            │   └── snapshot_cache.rs #   terminal page assembly、原子发布与有界 Host 查询索引
            └── inject_dll.ps1        #   已禁用的 legacy 注入脚本，仅保留历史证据，不是 fallback
```

---

## 三、模块依赖关系图

### 3.1 顶层模块依赖（宏观）

```
React pages -> api/client.ts -> Tauri invoke / Channel
                                  |
                                  v
                        Rust DomainService
                    / SessionManager / EventHub
                                  |
                         Named Pipe RPC v1
                                  |
                                  v
Main.cpp -> NamedPipeRpcServer -> CoreCommandService -> EngineFacade
    |                                      |               |
    +-> PostRenderHook -> GameThreadExecutor -> FrameScheduler -> Snapshot / Reflection capture
    +-> CoreRuntime / CapabilityRegistry                  SafeMemory / PropertyCodec
    +-> Generator / Engine / Platform
```

`Server/` 与 `API/` 不在上述依赖图中，因为 release Core 不编译它们。它们只供
历史审计和旧 API contract 对照；不得重新接入 Main 或前端。

### 3.2 DLL 启动流程（Main.cpp）

```
DllMain(DLL_PROCESS_ATTACH)
  └→ CreateThread(MainThread)
       │
       ├─ Settings::Config::Load()         读取 Dumper 引擎/生成配置
       ├─ PrimeGameVersionBeforeOffsetInit()   探测 UE 版本
       ├─ Generator::InitEngineCore()     ★ 引擎核心初始化
       │   ├─ ObjectArray::Init()          定位 GObjects
       │   ├─ FName::Init()               定位 GNames
       │   ├─ Off::InitRuntime()          仅发现基础运行/identity/Hook 偏移
       │   ├─ Off::InitPE_Windows()       定位 ProcessEvent
       │   └─ Off::InitPostRender_Windows() 定位 PostRender VTable
       │
       ├─ Publish EngineContext / EngineFacade / CoreCommandService
       ├─ Create ObjectSnapshotReflectionCandidateSource from immutable Context
       ├─ NamedPipeRpcServer::Start()      绑定 PID-scoped Pipe，尚未开放 admission
       ├─ PostRenderHook::Install()        安装生产 game-thread pump
       ├─ AttachFrameClient()              Backend 接入唯一 FrameScheduler
       ├─ FrameScheduler::AttachClient()   接入 snapshot producer
       ├─ Publish capabilities / Ready
       ├─ OpenAdmissions()                 仅在事实 Ready 后接收 Host
       └─ [稳定 Object Snapshot generation]
           └─ Prepare/attach witnessed reflection capture；终态 detach/release
       │
       └─ [Host Shutdown RPC 或当前 legacy F6 触发退出]
            ├─ Stop Named Pipe / settle requests
            ├─ Restore PostRender Hook
            ├─ Detach reflection/snapshot clients / FrameScheduler / stop facade
            ├─ Drain CoreRuntime request leases
            └─ 安全性可证明时 FreeLibraryAndExitThread()
```

`Off::InitReflection()`、GWorld/GEngine/FText/PropertySizes 探测和
`Generator::InitInternal()` 不属于基础启动；当前 release 不开放旧反射入口。
ReflectionLayout 由上述 immutable snapshot + SafeMemory 候选源独立采集，不会
回退到 legacy 全局 offset 初始化。其余可选领域只能由具备 capability witness、
游戏线程约束和独立失败状态的领域/生成任务显式激活。

### 3.3 Engine 层内部依赖

```
┌─────────────────────────────────────────────────────────────┐
│                    Engine 模块依赖图                          │
│                                                             │
│  ┌──────────┐     ┌────────────┐     ┌──────────────────┐  │
│  │  Enums   │◄────│UnrealTypes │◄────│ UnrealContainers │  │
│  │(标志枚举) │     │ (FName等)  │     │(TArray/FString等) │  │
│  └────┬─────┘     └─────┬──────┘     └──────────────────┘  │
│       │                 │                                    │
│       ▼                 ▼                                    │
│  ┌──────────────────────────────┐     ┌──────────────────┐  │
│  │      UnrealObjects           │     │    Offsets       │  │
│  │  UEObject → UEField          │◄────│ Off::* 偏移定义   │  │
│  │  → UEStruct → UEClass        │     └───────┬──────────┘  │
│  │  → UEFunction                │             │              │
│  │  → UEProperty (20+ 子类)     │     ┌───────┴──────────┐  │
│  └──────────┬───────────────────┘     │  OffsetFinder    │  │
│             │                         │ (自动偏移探测)     │  │
│     ┌───────┴────────┐               └──────────────────┘  │
│     ▼                ▼                                      │
│ ┌──────────┐  ┌──────────┐         ┌─────────────────────┐ │
│ │ObjectArray│  │NameArray │         │BlueprintDecompiler  │ │
│ │(GObjects) │  │(GNames)  │         │(字节码反编译)        │ │
│ └──────────┘  └──────────┘         └─────────────────────┘ │
│                                                             │
│ 外部依赖：Platform (模式扫描, 内存访问)                       │
│          Settings (配置读取)                                 │
│          Utils (编码, 工具函数)                               │
└─────────────────────────────────────────────────────────────┘
```

**Engine 核心类继承链：**

```
UEObject                              所有 UE 对象基类
  ├─ GetVft(), GetFlags(), GetIndex(), GetClass(), GetOuter()
  ├─ GetFName(), GetName(), GetFullName(), GetCppName()
  ├─ ProcessEvent(func, params)        通过 VTable 调用
  └─ IsA(class)
      │
      └─ UEField                      字段基类
          ├─ GetNext()
          │
          ├─ UEEnum                   枚举
          │   └─ GetNameValuePairs()
          │
          ├─ UEStruct                 结构体
          │   ├─ GetSuper(), GetChild(), GetChildProperties()
          │   ├─ GetProperties(), GetFunctions(), GetSize()
          │   └─ FindMember(name)
          │   │
          │   ├─ UEFunction           函数
          │   │   ├─ GetFunctionFlags(), GetScript()
          │   │   └─ GetReturnProperty()
          │   │
          │   └─ UEClass              类
          │       ├─ GetCastFlags(), GetDefaultObject()
          │       └─ GetFunction(name)
          │
          └─ UEProperty               属性基类 (20+ 子类)
              ├─ UEByteProperty       ├─ UEObjectProperty
              ├─ UEBoolProperty       ├─ UEClassProperty
              ├─ UEStructProperty     ├─ UEWeakObjectProperty
              ├─ UEArrayProperty      ├─ UELazyObjectProperty
              ├─ UEMapProperty        ├─ UESoftObjectProperty
              ├─ UESetProperty        ├─ UESoftClassProperty
              ├─ UEEnumProperty       ├─ UEInterfaceProperty
              ├─ UEDelegateProperty   ├─ UEFieldPathProperty
              ├─ UEMulticastInlineDelegateProperty
              └─ UEOptionalProperty
```

### 3.4 Generator 层内部依赖

```
┌────────────────────────────────────────────────────────────┐
│                 Generator 模块依赖图                        │
│                                                            │
│  ┌──────────────────────────────────┐                      │
│  │        Generator (基类)           │                      │
│  │  InitEngineCore() InitInternal() │                      │
│  │  Generate<T>() SetupFolders()    │                      │
│  └──────────┬───────────────────────┘                      │
│             │ 被 4 个生成器调用                               │
│    ┌────────┼──────────┬────────────┐                      │
│    ▼        ▼          ▼            ▼                      │
│ ┌────────┐┌─────────┐┌──────────┐┌──────────────┐         │
│ │CppGen  ││MappingG ││DumpspaceG││IDAMappingGen │         │
│ │(C++ SDK)│(USMAP)  ││(JSON)    ││(IDA 脚本)    │         │
│ └──┬─────┘└──┬──────┘└──┬───────┘└──────────────┘         │
│    │         │          │                                   │
│    └─────────┴──────────┘                                   │
│              │ 共同依赖                                      │
│    ┌─────────┼──────────────────────┐                      │
│    ▼         ▼                      ▼                      │
│ ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│ │PackageManager│  │StructManager │  │ MemberManager    │  │
│ │(包组织/依赖)  │  │(结构体索引)   │  │(成员名称解析)     │  │
│ └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
│        │                 │                    │             │
│        ▼                 ▼                    ▼             │
│ ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│ │DependencyMgr │  │  EnumManager │  │CollisionManager  │  │
│ │(拓扑排序)     │  │(枚举索引)    │  │(名称冲突处理)     │  │
│ └──────────────┘  └──────────────┘  └──────────────────┘  │
│                         │                    │             │
│              ┌──────────┴────────────────────┘             │
│              ▼                                             │
│  ┌──────────────────────────────────────┐                  │
│  │ Wrappers (StructWrapper,             │                  │
│  │  EnumWrapper, MemberWrappers)        │                  │
│  │  → 封装 UE 对象为生成器友好接口        │                  │
│  └──────────────────────────────────────┘                  │
│              │                                             │
│              ▼                                             │
│  ┌──────────────────────────────────────┐                  │
│  │ HashStringTable + PredefinedMembers  │                  │
│  │ (高性能字符串表 + 预定义结构体覆盖)    │                  │
│  └──────────────────────────────────────┘                  │
└────────────────────────────────────────────────────────────┘
```

### 3.5 Legacy API 层 → Engine/Generator 依赖矩阵（非 release）

```
┌─────────────┬──────────────────────────────────────────────────────────┐
│ API 模块     │ 依赖的 Engine/Generator/Platform 组件                    │
├─────────────┼──────────────────────────────────────────────────────────┤
│ StatusApi   │ ObjectArray, NameArray, Offsets, Settings, Generator    │
│ ObjectsApi  │ ObjectArray, UnrealObjects, UnrealTypes, NameArray,     │
│             │ UnrealContainers, Enums, Platform                       │
│ ClassesApi  │ ObjectArray, UnrealObjects, UnrealTypes, Enums          │
│ EnumsApi    │ ObjectArray, UnrealObjects, Enums                       │
│ DumpApi     │ Generator, CppGenerator, MappingGenerator,              │
│             │ DumpspaceGenerator, IDAMappingGenerator, Settings       │
│ MemoryApi   │ ObjectArray, UnrealObjects, WinMemApi                   │
│ WorldApi    │ ObjectArray, NameArray, UnrealObjects, UnrealContainers,│
│             │ Offsets, Platform, Settings                              │
│ CallApi     │ ObjectArray, UnrealObjects, UnrealTypes, Enums,         │
│             │ Offsets, GameThreadQueue                                 │
│ BlueprintApi│ ObjectArray, UnrealObjects, Enums, Offsets,             │
│             │ BlueprintDecompiler, Platform                            │
│ WatchApi    │ ObjectArray, UnrealObjects, UnrealTypes,                │
│             │ UnrealContainers → EventsApi (SSE 推送)                  │
│ HookApi     │ ObjectArray, UnrealObjects, UnrealTypes, Enums,         │
│             │ Offsets, GameThreadQueue, WinMemApi → EventsApi          │
│ EventsApi   │ HttpServer (通过 SetServer 注入)                         │
└─────────────┴──────────────────────────────────────────────────────────┘
```

### 3.6 Legacy API 间交叉依赖（非 release）

```
ObjectsApi ──────► 导出 ReadPropertyValueUnified, SerializePropertyUnified,
                   SerializeFunctionUnified 供 ClassesApi 复用

WatchApi ─────────► EventsApi::BroadcastWatchEvent()   属性变更 SSE 推送
HookApi ──────────► EventsApi::BroadcastHookEvent()    Hook 命中 SSE 推送
HookApi ──────────► GameThreadQueue                    PostRender 执行队列
CallApi ──────────► GameThreadQueue::Submit()           游戏线程调度

Router.cpp ───────► SetServer(&server) 注入 HttpServer 到 EventsApi
                  ► RegisterAll*Routes() 注册 13 个 API 模块
                  ► InitHooks() 安装 VTable 钩子
```

### 3.7 Legacy HookApi 调用链（历史实现）

```
HookApi::InitHooks()
  ├─ InstallPostRenderHook()
  │   └─ 对 GameViewportClient::PostRender VTable[98] 打补丁
  │       └─ HookedPostRender()
  │           ├─ GameThread::ProcessQueue()  ← 执行 CallApi 提交的任务
  │           └─ 调用原始 PostRender
  │
  └─ InstallPEVTableHook()
      └─ 遍历所有 UClass CDO → ProcessEvent VTable 打补丁
          └─ HookedProcessEvent()
              ├─ 检查 g_MonitoredFunctions
              ├─ 记录 HookLogEntry (调用者、时间)
              ├─ EventsApi::BroadcastHookEvent() → SSE 推送
              └─ 调用原始 ProcessEvent

CallApi 游戏线程调度流程:
  POST /call/function {use_game_thread: true}
    └─ GameThread::Submit(obj, func, params)
        └─ 阻塞等待 → HookedPostRender 中 ProcessQueue() 执行
            └─ 原始 ProcessEvent(obj, func, params)
                └─ 返回结果 → HTTP 响应
```

### 3.8 Platform 层被依赖关系

```
Platform (PlatformWindows + Arch_x86)
  │
  ├─ 被 OffsetFinder 使用 → 模式扫描、字符串搜索、VTable 遍历
  ├─ 被 ObjectArray 使用 → GObjects 地址校验
  ├─ 被 NameArray 使用 → GNames 地址定位
  ├─ 被 Offsets 使用 → ProcessEvent/GWorld/GEngine 扫描
  ├─ 被 ObjectsApi 使用 → IsBadReadPtr 安全检查
  ├─ 被 WorldApi 使用 → 地址有效性检查
  └─ 被 BlueprintApi 使用 → 安全内存读取

PlatformWindows 核心函数:
  GetModuleBase()                  获取主模块基址
  FindPattern(pattern)             AOB 模式扫描
  FindByStringInAllSections(str)   字符串引用扫描
  FindAlignedValueInAllSections()  对齐值扫描
  IsBadReadPtr(addr, size)         安全内存检查 (try-catch)
  IterateVTableFunctions()         VTable 函数遍历

Arch_x86 核心函数:
  Resolve32BitRelativeCall()       解析相对调用
  Resolve32BitRIPRelativeJump()    解析 RIP 相对跳转
  FindFunctionEnd()                查找函数边界
```

---

## 四、当前前端 → Host → Core 通信矩阵

### 4.1 通信架构

```
React UExplorerApi
  ├─ invoke("domain_request", { request })
  ├─ invoke("inject_and_connect", ...)
  ├─ invoke("disconnect_session", ...)
  └─ invoke("subscribe_session_events", Channel)
                    |
                    v
Rust Host: DomainService / SessionManager / EventBridgeManager
                    |
                    v
       \\.\pipe\UExplorer\v1\<pid>
                    |
                    v
Core: NamedPipeRpcServer -> CoreCommandService -> EngineFacade
```

无 localhost、port、Token、`runtime.ini`、HTTP、SSE 或 WebSocket 桌面运行路径。
Vite 的开发资产 URL 不属于 React -> Core 通信。

### 4.2 前端页面 → Domain operation 映射

```
Dashboard.tsx
  ├─ getStatus()              status.inspect
  ├─ getObjectCounts()        objects.count
  └─ session events           Tauri Channel

Objects.tsx (三面板)
  ├─ objects.list/search/get_*      immutable Host snapshot + full path identity
  ├─ types.{packages|classes|structs|enums}.list  generation/query-bound cursor
  ├─ types.packages.contents / types.classes.instances  exact path + cursor (1..128)
  ├─ objects.property.read          exact ObjectHandle + TypeSnapshot generation
  └─ property write/复杂 codec -> CAPABILITY_UNAVAILABLE

Functions.tsx (四合一)
  ├─ types.functions.get            exact full path + immutable FunctionHandle
  ├─ call.invoke                    exact target/function handle + owned ParamFrame
  ├─ static call target             types.classes.cdo -> explicit CDO handle
  ├─ hook events                    filtered Tauri Channel
  └─ batch/Hook/Blueprint operation -> R5 capability gate

WorldBrowser.tsx
  ├─ world.inspect / world.levels / world.actors.list  immutable WorldSnapshot + cursor
  ├─ Actor 按 exact level.full_path 分组；Level/Actor page 显式 load-more
  ├─ world.actor.get                  exact ActorHandle + WorldSnapshot generation
  ├─ world.actor.components           Actor-bound cursor page + explicit load-more
  ├─ world.shortcuts                  exact UWorld relations + witnessed LocalPlayers[0] 链；缺 metadata 显式 unavailable
  ├─ objects.property.read × 3        exact RootComponent + TypeSnapshot 的 stored RelativeLocation/Rotation/Scale
  └─ transform update -> world.mutate capability gate

Memory.tsx
  ├─ watch events -> filtered Tauri Channel
  └─ Memory/Watch operation -> R5 capability gate

SDKDump.tsx
  └─ Dump operation -> R5 capability gate

Settings.tsx
  ├─ updateSettings()         本地 UI preference
  ├─ getEngineStatus()        status.engine
  └─ 无端口/Token/连接文件设置
```

### 4.3 实时通道

```
Core bounded Event writer
  -> Named Pipe Event frame (seq/session/drop metadata)
  -> per-session Rust EventHub (bounded replay/filter/fan-out)
  -> caller-owned Tauri Channel
  -> React page-local reconciliation
```

Watch/Hook producer 尚未在 R5 开放，因此“通道存在”不等于这些领域功能可用。

---

## 五、关键数据流

本章保留 R4 前的领域实现数据流，用于定位 R5 需要替换的直接 live-memory、
旧 API 和旧 Hook 路径；它不是当前可调用面的说明。

### 5.1 对象属性读取流

```
前端 GET /objects/:index/properties
  → ObjectsApi handler
    → ObjectArray::GetByIndex(index) 获取 UEObject
      → UEObject::GetClass() 获取 UEClass
        → UEClass::GetProperties() 遍历 FProperty 链
          → 逐属性: ReadPropertyValue(obj, prop)
            → 根据属性类型分派:
              Bool → 读取 FieldMask + ByteOffset
              Int/Float/Double → 直接 SafeReadValue
              FName → FName::ToString()
              FString → 读取 TArray<wchar_t> → wstring
              Object → 读取指针 → 解析对象名
              Array → TryReadArrayHeader → 逐元素读取
              Struct → 递归读取子属性
            → JSON 序列化返回
```

### 5.2 SDK 生成流

```
前端 POST /dump/sdk
  → DumpApi handler
    → LaunchGeneratorJob<CppGenerator>() 创建异步线程
      → CppGenerator::Generate()
        → PackageManager 遍历所有包
          → 逐包:
            StructWrapper 遍历结构体/类
              → MemberWrappers 遍历属性
                → CollisionManager 处理命名冲突
                  → DependencyManager 排序依赖
                    → 写入 .h 文件到 CppSDK/
        → DumpJob 状态更新 (Running → Completed/Failed)
          → 前端轮询 GET /dump/jobs/:id
```

### 5.3 Hook 触发流

```
UE 游戏调用某个 UFunction
  → 进入 HookedProcessEvent (VTable 被替换)
    → 查询 g_MonitoredFunctions 是否命中
      → 是: 记录 HookLogEntry + BroadcastHookEvent()
        → SSE: event: hook_hit, data: {hookId, funcName, caller, timestamp}
          → 前端 Functions 页 Hook Tab 实时显示
      → 否: 直接转发到原始 ProcessEvent
```

---

## 六、线程模型

```
┌─────────────────────────────────────────────────────────────────────┐
│                        游戏进程内线程分布                              │
│                                                                     │
│  [游戏主线程]                                                        │
│    └─ Tick → Render → PostRender                                    │
│        └─ PostRenderHook callback                                   │
│            ├─ GameThreadExecutor::ProcessQueue()                    │
│            ├─ GameThreadFrameScheduler (32 units / 2 ms)            │
│            │   └─ snapshot / future reflection-type-watch clients   │
│            └─ original PostRender                                   │
│                                                                     │
│  [DLL 主线程] (CreateThread from DllMain)                            │
│    └─ MainThread()                                                  │
│        ├─ 引擎初始化                                                  │
│        ├─ capability/readiness 刷新                                  │
│        └─ Host Shutdown / 当前 legacy F6 退出监听                     │
│                                                                     │
│  [Named Pipe listener]                                              │
│    ├─ overlapped session reader                                     │
│    ├─ 4 个有界 request worker                                        │
│    └─ 1 个有界 Event writer                                          │
│                                                                     │
│  [Dump Worker 线程] (LaunchGeneratorJob)                             │
│    └─ CppGenerator::Generate() / MappingGenerator::Generate() / ... │
│                                                                     │
│  同步机制:                                                            │
│    bounded GameThread MPSC + ticket/cancel/deadline                  │
│    VTableHookToken + CallbackBarrier                                │
│    bounded Pipe request/Event queues                                │
│    CoreRuntime request lease + ShutdownCoordinator                  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 七、Legacy API v1 端点清单（历史快照，非运行路径）

本章保留旧 HTTP 端点，目的是审计迁移覆盖和运行
`tests/contracts/verify-api-v1.ps1`。Release Core 不注册这些端点；当前可调用面以
Rust `DomainService` 的显式 operation registry 为准。

### 状态与连接
| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/status` | DLL 状态、UE 版本、地址、对象数量 |
| GET | `/status/health` | 心跳检测（免认证） |
| GET | `/status/engine` | 详细引擎信息（offsets/addresses/internals） |
| POST | `/status/reconnect` | 重新扫描 GObjects/GNames |

### 对象枚举
| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/objects` | 分页对象列表（q/offset/limit） |
| GET | `/objects/count` | 按类型统计数量 |
| GET | `/objects/search` | 多条件搜索（q/class/package） |
| GET | `/objects/:index` | 单对象详情 |
| GET | `/objects/:index/properties` | 对象属性列表 |
| GET | `/objects/:index/outer-chain` | Outer 链 |
| GET | `/objects/:index/property/:name` | 读取单个属性值 |
| POST | `/objects/:index/property/:name` | 写入单个属性值 |
| GET | `/objects/by-address/:addr` | 按地址查找 |
| GET | `/objects/by-path/:path` | 按路径查找 |
| GET | `/packages` | 包列表 |
| GET | `/packages/:name/contents` | 包内对象 |

### 类型系统
| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/classes` | 类列表 |
| GET | `/classes/:name` | 类详情（fields + functions） |
| GET | `/classes/:name/fields` | 字段列表 |
| GET | `/classes/:name/functions` | 函数列表 |
| GET | `/classes/:name/hierarchy` | 继承链 |
| GET | `/classes/:name/instances` | 实例列表 |
| GET | `/classes/:name/cdo` | CDO 属性值 |
| GET | `/structs` | 结构体列表 |
| GET | `/structs/:name` | 结构体详情 |
| GET | `/enums` | 枚举列表 |
| GET | `/enums/:name` | 枚举值列表 |

### SDK 生成
| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/dump/sdk` | 生成 C++ SDK |
| POST | `/dump/usmap` | 生成 USMAP |
| POST | `/dump/dumpspace` | 生成 Dumpspace JSON |
| POST | `/dump/ida-script` | 生成 IDA 脚本 |
| GET | `/dump/jobs` | 任务列表 |
| GET | `/dump/jobs/:id` | 任务状态 |

### 内存操作
| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/memory/read` | 原始字节读取 |
| POST | `/memory/read-typed` | 类型化读取 |
| POST | `/memory/write` | 原始字节写入 |
| POST | `/memory/write-typed` | 类型化写入 |
| POST | `/memory/pointer-chain` | 指针链跟踪 |

### 函数调用
| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/call/function` | 调用 UFunction |
| POST | `/call/static` | 通过 CDO 调用静态函数 |
| POST | `/call/batch` | 批量调用 |

### Hook 管理
| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/hooks/add` | 添加 Hook |
| DELETE | `/hooks/:id` | 移除 Hook |
| PATCH | `/hooks/:id` | 启用/禁用 |
| GET | `/hooks/list` | Hook 列表 |
| GET | `/hooks/:id/log` | 调用日志 |

### 属性监视
| Method | Endpoint | 说明 |
|--------|----------|------|
| POST | `/watch/add` | 添加监视 |
| DELETE | `/watch/:id` | 移除监视 |
| GET | `/watch/list` | 监视列表 |
| GET | `/watch/:id/history` | 变更历史 |

### 蓝图反编译
| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/blueprint/decompile?index=N` | 按索引反编译 |
| GET | `/blueprint/bytecode?index=N` | 按索引字节码 |
| GET | `/blueprint/:funcpath/decompile` | 按路径反编译 |
| GET | `/blueprint/:funcpath/bytecode` | 按路径字节码 |

### 世界与 Actor
| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/world` | 当前 UWorld |
| GET | `/world/levels` | 已加载 Level |
| GET | `/world/actors` | Actor 列表（分页/过滤） |
| GET | `/world/actors/:index` | Actor 详情 |
| GET | `/world/actors/:index/components` | 组件列表 |
| POST | `/world/actors/:index/transform` | 修改 Transform |
| GET | `/world/debug/fname` | FName 调试 |
| GET | `/world/shortcuts` | GameMode/GameState/PC/Pawn |

### 实时事件
| Method | Endpoint | 说明 |
|--------|----------|------|
| GET | `/events/stream` | 统一 SSE 流 |
| GET | `/events/watches` | Watch SSE 流 |
| GET | `/events/hooks` | Hook SSE 流 |
| WS | `/ws/console` | Console WebSocket |
| WS | `/ws/events` | 事件 WebSocket |

---

## 八、命名空间汇总

| 命名空间 | 用途 | 所在文件 |
|----------|------|----------|
| `UExplorer` | 主命名空间（HttpServer 等） | Server/ |
| `UExplorer::API` | REST API 路由与公共工具 | API/ |
| `UExplorer::GameThread` | 游戏线程调度队列 | GameThreadQueue.h |
| `Settings::*` | 配置（General/Config/PostRender/EngineCore/Generator） | Settings.h |
| `Off::*` | 引擎偏移（30+ 子命名空间） | Offsets.h |
| `Off::InSDK::*` | SDK 运行时偏移（ProcessEvent/World/ObjArray/Name） | Offsets.h |
| `OffsetFinder` | 偏移自动探测 | OffsetFinder.h |
| `PlatformWindows` | Windows 平台实现 | PlatformWindows.h |
| `Architecture_x86_64` | x86-64 架构工具 | Arch_x86.h |
| `UC` | Unreal 容器（TArray/TMap/FString） | UnrealContainers.h |
| `PackageManagerUtils` | 包依赖工具 | PackageManager.h |
| `KeyFunctions` | 碰撞信息键生成 | CollisionManager.h |
| `PropertySizes` | 属性大小常量 | Offsets.h |
| `Utils` | 通用工具 | TmpUtils.h |
| `FileNameHelper` | 文件名处理 | TmpUtils.h |

---

## 九、项目进度状态

| 阶段 | 状态 | 内容 |
|------|------|------|
| R0 证据与测试地基 | **已完成** | 128 项 issue register、协议/contract/harness/CI |
| R1 安全止血 | **实现阶段完成** | 注入、队列、Hook、内存与卸载关键风险止血；UE 环境证据进 R7 |
| R2 CoreRuntime/能力模型 | **实现阶段完成** | Runtime、Context、Capability、Handle、Snapshot、SafeMemory |
| R3 Named Pipe/Rust Host | **实现阶段完成** | 严格 IPC、SessionManager、EventHub、注入与跨语言 fixture |
| R4 通信原子切换 | **已完成** | React 只走 Tauri；Core release 只走 Named Pipe，无网络栈 |
| R5 领域正确性 | **当前阶段（R5.3）** | Property read、单目标 Call、单层 Array descriptor、exact FVector/FRotator descriptor、World 列表/Actor detail/Component page/local-player shortcuts 与存储 Relative* 读取已接入；同帧/computed transform、setter、Memory/Watch/Hook/Blueprint/Dump 待办 |
| R6 前端状态重构 | **未开始** | session store、query lifecycle、BigInt 地址、能力驱动 UI |
| R7 发布硬化 | **未开始** | UE fixture、性能/压力、卸载、发布与文档门禁 |

旧 Phase 1-5 的“功能已完成”结论已经废止；界面或 legacy handler 存在不代表能力
正确。逐问题状态见 `docs/issue-status.json`，验收门见 `REFACTOR_PLAN.md`。
