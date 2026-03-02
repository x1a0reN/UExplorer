# UExplorer — 功能设计与页面架构方案

## Context

基于 Dumper-7 的实现原理，设计一个桌面端 Unreal Engine SDK Dump + 实时探索工具。
- 主要功能：SDK Dump（类似 Dumper7）
- 辅助功能：Live Explorer（类似 UE4SS / UnityExplorer，但更强大）
- 架构：Tauri 2 + React + TS + Vite 前端，C++ DLL 核心（注入/劫持），HTTP 通信
- 第一阶段：功能设计 + 页面功能设计（用户负责 UI 设计）

---

## 一、系统架构

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

### Page 2: Objects（对象浏览器）

**定位：** 所有对象/类/结构体/枚举/Actor 的查看和编辑

**覆盖 DLL API：** Objects API、Classes API、Enums API、World API、Memory API（写属性）、Watch API

**功能区域：**

1. **搜索与过滤栏**（顶部）
   - 搜索框（支持名称/路径搜索）
   - 类型切换 Tab：All / Class / Struct / Enum / Function / Package / Actor
   - 包过滤器（输入包名过滤）
   - 对象标志过滤（RF_Public, RF_Standalone 等）

2. **左侧面板 — 对象列表**
   - 虚拟滚动列表，支持 5 万+ 对象
   - 每行显示：索引、名称、类名、地址（紧凑格式）
   - 总数 + 当前过滤匹配数
   - 双击或回车打开详情

3. **右侧面板 — 详情**（多 Tab 切换）
   - **Info Tab**：名称、完整路径、地址、类名、Outer 链、Index、Flags、内存大小
   - **Properties Tab**：属性表格
     - 列：名称、类型、值
     - 值可点击编辑（调用 Property Write API）
     - 类型感知渲染：FVector 显示 (X,Y,Z)、颜色显示色块、对象指针显示名称
     - Watch 按钮：添加到监视列表
     - 展开嵌套：结构体/数组/Map 可内联展开
     - CDO 对比：高亮与默认值不同的属性
   - **Fields Tab**（仅类/结构体）：字段表格
     - 列：偏移、名称、类型、大小、Flags
     - 按偏移排序
     - "仅本类" / "含继承字段" 切换
     - 点击类型跳转到该类型定义
   - **Functions Tab**（仅类）：函数列表
     - 列：名称、Flags、参数数量
     - 可点击跳转到 Functions 页面
   - **Instances Tab**（仅类）：该类的所有实例列表
     - 列：名称、地址
     - 可点击跳转到详情
   - **World Tab**（仅 Actor/ActorComponent）：世界信息
     - 位置：Location (X,Y,Z)
     - 旋转：Rotation (P,Y,R)
     - 缩放：Scale (X,Y,Z)
     - 组件树层级

4. **右键菜单**
   - 复制地址
   - 复制路径
   - 在新标签页打开
   - 添加到收藏夹

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

## 四、HTTP API 设计

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
    │   └── HttpServer.cpp             # WinSock2 实现 (路由匹配/Token认证/CORS)
    │
    ├── API\                           # REST API 路由层
    │   ├── ApiCommon.h                # JSON 响应信封 (MakeResponse/MakeError/ParseQuery/GetPathSegment)
    │   ├── Router.h / Router.cpp      # 路由注册中心 (RegisterAllRoutes)
    │   ├── StatusApi.h / .cpp         # GET /status, /status/health
    │   ├── ObjectsApi.h / .cpp        # GET /objects, /objects/count, /objects/:index, /objects/:index/properties
    │   ├── ClassesApi.h / .cpp        # GET /classes, /classes/:name, fields, functions, hierarchy, /structs
    │   ├── EnumsApi.h / .cpp          # GET /enums, /enums/:name
    │   ├── DumpApi.h / .cpp           # POST /dump/sdk,usmap,dumpspace,ida-script + GET /dump/jobs
    │   └── MemoryApi.h / .cpp         # POST /memory/read, /memory/read-typed, /memory/pointer-chain
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

## 六、开发阶段规划

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
- [ ] SDK Dump Center 页面 — 前端
- [ ] DLL 注入/劫持机制（Tauri 端）

### Phase 3: Explorer 基础（当前阶段）

**目标：** 实现实时对象浏览和类型检查

- [ ] Object Browser 页面（对象列表 + 属性查看）— 前端
- [ ] Class Inspector 页面（继承树 + 字段/函数详情）— 前端
- [x] 内存读取 API — `POST /memory/read`, `/memory/read-typed`, `/memory/pointer-chain`
- [x] 属性值读取（按名称自动解析偏移）— `GET /objects/:index/properties`，支持 Bool/Int/Float/Double/FName/FString/Object/Array 等
- [x] 单对象详情 API — `GET /objects/:index`，含 outer chain
- [x] Blueprint 反编译偏移修复 — Script offset fallback 机制
- [ ] 基础 Console 页面 — 前端

### Phase 4: 高级 Explorer

**目标：** 实现运行时交互能力（DLL 端）

- [x] 内存写入 + 属性值编辑 — `POST /memory/write`, `POST /objects/:index/property/:name`
- [x] ProcessEvent 函数调用 — `POST /call/function`
- [x] 属性监视（Watch）功能 — `/watch/add`, `/watch/list`, SSE 实时推送
- [ ] Function Browser 页面（含调用器）— 前端
- [x] World Explorer API — `/world`, `/world/levels`, `/world/actors`, `/world/shortcuts`
- [ ] Memory Viewer 页面 — 前端

### Phase 5: 进阶功能

**目标：** Hook、蓝图反编译等高级特性（DLL 端）

- [x] Hook Manager（UFunction Hook + SSE 实时推送）— `/hooks/*`, SSE 推送
- [ ] Hook Manager 页面 — 前端
- [x] Blueprint Decompiler — `/blueprint/:func/bytecode`, `/blueprint/:func/decompile`
- [ ] Blueprint Decompiler 页面 — 前端
- [x] IDA/Ghidra 导入脚本生成 — `/dump/ida-script`
- [x] Dumpspace JSON 生成 — `/dump/dumpspace`

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
