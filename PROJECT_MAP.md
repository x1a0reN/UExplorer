# UExplorer 项目全量地图

## 一、项目概述

UExplorer 是一个面向 Unreal Engine 的 **SDK Dump + 实时游戏内省工具**，核心理念是将 Dumper-7 的离线 dump 能力升级为在线实时探索体验。

**技术架构：三层分离**

```
┌──────────────────────────────────────────────┐
│  Frontend (Tauri 2 + React + TypeScript)     │  ← 桌面 GUI
│  6 页面 / 16 个前端文件 / ~1150 行 API 封装     │
└───────────────┬──────────────────────────────┘
                │ HTTP REST + SSE + WebSocket
┌───────────────┴──────────────────────────────┐
│  Core DLL (C++, 注入游戏进程)                   │  ← 引擎内核
│  95+ 源文件 / 13 个 API 模块 / 4 种 SDK 生成器    │
└───────────────┬──────────────────────────────┘
                │ 内存直接访问
┌───────────────┴──────────────────────────────┐
│  Target UE Game Process (UE 4.11 ~ 5.x)     │
└──────────────────────────────────────────────┘
```

---

## 二、文件树（95 个 C++ 源文件 + 16 个前端文件）

```
UExplorer/
├── CLAUDE.md                           # Agent 规则
├── DESIGN.md                           # 功能设计文档（727 行）
├── PROJECT_MAP.md                      # 本文档
├── UExplorerCore.slnx                  # VS2026 解决方案
│
├── Dumper/                             ★ Core DLL（C++）
│   ├── UExplorerCore.vcxproj          # MSBuild 项目文件 (v145, C++latest)
│   ├── Main.cpp                       # DLL 入口 (DllMain → MainThread)
│   ├── Settings.h / .cpp             # 全局配置管理
│   ├── TmpUtils.h                    # 工具函数（Align, StrToLower, MakeValidFileName）
│   │
│   ├── Server/                        ★ HTTP 服务器层
│   │   ├── HttpServer.h              #   PIMPL 接口（HttpRequest/HttpResponse/RouteHandler/SSE/WS）
│   │   └── HttpServer.cpp            #   WinSock2 实现（路由匹配/Token/CORS/SSE/WebSocket）
│   │
│   ├── API/                           ★ REST API 层（13 模块）
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
│   │   │   ├── Offsets.h             #   Off::* 全部偏移定义 (30+ 命名空间)
│   │   │   └── OffsetFinder.h        #   自动偏移探测函数 (FindUObjectClassOffset 等)
│   │   ├── Public/Blueprint/
│   │   │   ├── EExprToken.h          #   Blueprint VM 操作码枚举 (80+ 操作码)
│   │   │   └── BlueprintDecompiler.h #   字节码反编译器 (Decompile/ParseExpression)
│   │   └── Private/                  #   对应 .cpp 实现
│   │       ├── Blueprint/BlueprintDecompiler.cpp
│   │       ├── OffsetFinder/OffsetFinder.cpp, Offsets.cpp
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
│   │   │   └── Architecture.h        #   架构选择器 (→ Arch_x86)
│   │   └── Private/
│   │       ├── PlatformWindows.h/.cpp #   模式扫描, IsBadReadPtr, VTable 遍历, 内存地址校验
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
    │   ├── api/index.ts              #   UExplorerApi 类 (~1150 行，覆盖全部 50+ API 端点)
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
    │           ├── WorldBrowser.tsx   #     世界浏览 (Actor/Level)
    │           └── shared.tsx        #     共享组件/工具
    └── src-tauri/
        ├── Cargo.toml                #   Rust 依赖
        ├── tauri.conf.json           #   Tauri 应用配置
        ├── build.rs                  #   Tauri 构建脚本
        ├── capabilities/default.json #   Tauri 权限配置
        └── src/
            ├── main.rs               #   Tauri 主入口
            ├── lib.rs                #   Tauri 命令 (scan_ue_processes, inject_dll, save_connection_settings)
            └── inject_dll.ps1        #   PowerShell DLL 注入脚本
```

---

## 三、模块依赖关系图

### 3.1 顶层模块依赖（宏观）

```
                            ┌─────────────┐
                            │  Main.cpp   │  DLL 入口
                            │  (DllMain)  │
                            └──────┬──────┘
                   ┌───────────────┼───────────────┐
                   ▼               ▼               ▼
            ┌────────────┐  ┌───────────┐  ┌────────────┐
            │ Generator  │  │ HttpServer│  │  Settings  │
            │ (引擎初始化) │  │ (HTTP层)  │  │  (配置)    │
            └──────┬─────┘  └─────┬─────┘  └────────────┘
                   │              │
                   │        ┌─────┴─────┐
                   │        │  Router   │  路由注册中心
                   │        └─────┬─────┘
                   │              │ 注册 13 个 API 模块
            ┌──────┼──────────────┼──────────────────────┐
            ▼      ▼              ▼                      ▼
     ┌──────────┐ ┌──────────┐ ┌──────────┐      ┌──────────┐
     │StatusApi │ │ObjectsApi│ │ClassesApi│ ...  │EventsApi │
     │DumpApi   │ │MemoryApi │ │WorldApi  │      │HookApi   │
     └────┬─────┘ └────┬─────┘ └────┬─────┘      └────┬─────┘
          │            │            │                   │
          └────────────┴────────────┴───────────────────┘
                              │
                  ┌───────────┼───────────┐
                  ▼           ▼           ▼
           ┌───────────┐ ┌────────┐ ┌──────────┐
           │  Engine   │ │Generator│ │ Platform │
           │(UE 内省)  │ │(SDK生成)│ │(系统抽象) │
           └───────────┘ └────────┘ └──────────┘
```

### 3.2 DLL 启动流程（Main.cpp）

```
DllMain(DLL_PROCESS_ATTACH)
  └→ CreateThread(MainThread)
       │
       ├─ LoadConnectionConfig()          从 connection.ini 读取端口/Token
       ├─ PrimeGameVersionBeforeOffsetInit()   探测 UE 版本
       ├─ Generator::InitEngineCore()     ★ 引擎核心初始化
       │   ├─ ObjectArray::Init()          定位 GObjects
       │   ├─ FName::Init()               定位 GNames
       │   ├─ Off::Init()                 计算所有偏移
       │   ├─ PropertySizes::Init()       属性大小初始化
       │   ├─ Off::InitPE_Windows()       定位 ProcessEvent
       │   ├─ Off::InitGWorld()           定位 GWorld
       │   ├─ Off::InitGEngine()          定位 GEngine
       │   ├─ Off::InitTextOffsets()      定位 FText 布局
       │   └─ Off::InitPostRender_Windows() 定位 PostRender VTable
       │
       ├─ Generator::InitInternal()       ★ 类型系统索引
       │   ├─ PackageManager::Init()
       │   ├─ StructManager::Init()
       │   ├─ EnumManager::Init()
       │   ├─ MemberManager::Init()
       │   └─ PackageManager::PostInit()
       │
       ├─ HttpServer(port, token)          创建 HTTP 服务器
       ├─ RegisterAllRoutes(server)        注册全部 API 路由 + InitHooks
       ├─ server.Start()                   开始监听
       │
       └─ [F6 退出循环]
            ├─ ShutdownHooks()
            ├─ server.Stop()
            └─ FreeLibraryAndExitThread()
```

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

### 3.5 API 层 → Engine/Generator 依赖矩阵

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

### 3.6 API 间交叉依赖

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

### 3.7 Hook 机制的调用链

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

## 四、前端 → 后端通信矩阵

### 4.1 通信架构

```
┌──────────────────┐    HTTP REST     ┌──────────────────┐
│                  │◄────────────────►│                  │
│    Frontend      │    SSE (单向)     │   Core DLL       │
│  (Tauri + React) │◄─────────────────│  (注入游戏进程)   │
│                  │   WebSocket (双向) │                  │
│                  │◄────────────────►│                  │
└───────┬──────────┘                  └──────────────────┘
        │
        │ Tauri invoke (Rust ↔ JS)
        ▼
┌──────────────────┐
│  Tauri Backend   │
│  (Rust)          │
│  - scan_ue_processes    进程扫描
│  - inject_dll           DLL 注入
│  - save_connection_settings  配置持久化
│  - load_runtime_endpoint     运行时端点发现
└──────────────────┘
```

### 4.2 前端页面 → API 端点映射

```
Dashboard.tsx
  ├─ getStatus()              GET  /status
  ├─ getObjectCounts()        GET  /objects/count
  ├─ getWorldShortcuts()      GET  /world/shortcuts
  └─ healthCheck()            GET  /status/health

Objects.tsx (三面板)
  ├─ HierarchyPane → getClasses(), getStructs(), getEnums()
  ├─ InstancePane  → getClassInstances(name)
  └─ InspectorPane → getObjectProperties(idx), setObjectProperty(idx, name, val)
                     getClassByName(name), getClassCDO(name)

Functions.tsx (四合一)
  ├─ 函数列表     → getClassFunctions(name)
  ├─ Call Tab     → callFunction(), callStaticFunction(), callFunctionBatch()
  ├─ Hook Tab     → addHook(), listHooks(), removeHook(), setHookEnabled(), getHookLog()
  │                 subscribeEventStream('/events/hooks')
  └─ Decompile Tab → decompileBlueprint(idx), getBlueprintBytecode(idx)

Memory.tsx
  ├─ Hex 视图      → readMemory(addr, size), writeMemory(addr, bytes)
  ├─ 类型化读写    → readTypedMemory(addr, type), writeTypedMemory(addr, type, val)
  ├─ 指针链        → resolvePointerChain(base, offsets)
  ├─ Console       → connectWebSocket('/ws/console')
  └─ Watch 面板    → addWatch(), listWatches(), removeWatch(), getWatchHistory()
                     subscribeEventStream('/events/watches')

SDKDump.tsx
  ├─ startDump(type)          POST /dump/{sdk|usmap|dumpspace|ida-script}
  ├─ getDumpJobs()            GET  /dump/jobs
  └─ getDumpJob(id)           GET  /dump/jobs/:id

Settings.tsx
  ├─ updateSettings()         本地 localStorage
  ├─ persistConnectionSettings()  Tauri invoke
  ├─ getEngineStatus()        GET  /status/engine
  └─ reconnectEngine()        POST /status/reconnect
```

### 4.3 实时通道

```
SSE 端点 (服务端 → 客户端, 单向):
  /events/stream    → 全部事件（Dashboard 全局监听）
  /events/hooks     → Hook 命中事件（Functions 页 Hook Tab）
  /events/watches   → Watch 变更事件（Memory 页 Watch 面板）

WebSocket 端点 (双向):
  /ws/console       → Console 命令交互（Memory 页 Console 面板）
  /ws/events        → 统一实时事件流（SSE 的 WebSocket 替代）

认证:
  HTTP:  X-UExplorer-Token header
  SSE:   X-UExplorer-Token header (通过 fetch stream)
  WS:    ?token= 查询参数
```

---

## 五、关键数据流

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
│        └─ HookedPostRender()                                        │
│            ├─ GameThread::ProcessQueue()  ← 执行 HTTP 端提交的任务    │
│            └─ 原始 PostRender                                        │
│                                                                     │
│  [DLL 主线程] (CreateThread from DllMain)                            │
│    └─ MainThread()                                                  │
│        ├─ 引擎初始化                                                  │
│        └─ F6 退出监听循环                                              │
│                                                                     │
│  [HTTP Accept 线程] (HttpServer::Start)                              │
│    └─ AcceptLoop() → 每个连接:                                       │
│        ├─ [HTTP Worker 线程] HandleClient()                          │
│        │   └─ MatchAndHandle(req) → API handler                     │
│        ├─ [SSE 长连接线程] 保持连接 + 心跳                              │
│        └─ [WebSocket 线程] HandleWebSocketClient()                   │
│                                                                     │
│  [Dump Worker 线程] (LaunchGeneratorJob)                             │
│    └─ CppGenerator::Generate() / MappingGenerator::Generate() / ... │
│                                                                     │
│  [Watch 轮询线程] (WatchApi 内部)                                     │
│    └─ 定时检查属性值变化 → BroadcastWatchEvent                         │
│                                                                     │
│  同步机制:                                                            │
│    g_QueueMutex + g_QueueCV     (GameThreadQueue)                   │
│    g_HookMutex                  (HookApi: hook 注册/查询)            │
│    g_LogMutex                   (HookApi: 日志写入)                   │
│    g_PEVTableMutex              (HookApi: VTable 修补)               │
│    sScriptOffsetDiagnosticsMutex (OffsetFinder: 诊断数据)             │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 七、完整 API 端点清单（50+ 端点）

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
| Phase 1: 基础框架 | **已完成** | DLL + HTTP Server + 基础 API + 首次编译验证 |
| Phase 2: SDK Dump | **已完成** | 4 种格式生成器 + 任务管理 + Dashboard/SDKDump 页面 |
| Phase 3: Explorer 基础 | **当前阶段** | 对象浏览器 + 属性读取 + Blueprint 反编译偏移修复 |
| Phase 4: 高级 Explorer | **已完成** | 内存读写 + 函数调用 + Watch + Hook + World Explorer |
| Phase 5: 进阶功能 | **已完成** | Hook Manager + WebSocket + Blueprint 反编译 + IDA 脚本 |

**未完成项：**
- [ ] Class Inspector 独立继承树页面
- [ ] DLL 注入/劫持机制（Tauri 端完善）
- [ ] 前端 HTTP Client 封装优化（React Query 缓存/重试）
