$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Read-ProjectFile([string]$relativePath) {
    return Get-Content -LiteralPath (Join-Path $root $relativePath) -Raw -Encoding UTF8
}

function Assert-Contains([string]$text, [string]$token, [string]$message) {
    if (-not $text.Contains($token)) {
        throw "$message Missing token: $token"
    }
}

function Assert-NotContains([string]$text, [string]$token, [string]$message) {
    if ($text.Contains($token)) {
        throw "$message Forbidden token: $token"
    }
}

$runtime = Read-ProjectFile 'Dumper\Runtime\CoreRuntime.h'
$coreSession = Read-ProjectFile 'Dumper\Runtime\CoreSession.cpp'
$context = Read-ProjectFile 'Dumper\Runtime\EngineContext.h'
$capture = Read-ProjectFile 'Dumper\Runtime\EngineContextCapture.cpp'
$offsetsHeader = Read-ProjectFile 'Dumper\Engine\Public\OffsetFinder\Offsets.h'
$offsets = Read-ProjectFile 'Dumper\Engine\Private\OffsetFinder\Offsets.cpp'
$generator = Read-ProjectFile 'Dumper\Generator\Private\Generators\Generator.cpp'
$nameArrayHeader = Read-ProjectFile 'Dumper\Engine\Public\Unreal\NameArray.h'
$nameArray = Read-ProjectFile 'Dumper\Engine\Private\Unreal\NameArray.cpp'
$nameCodecHeader = Read-ProjectFile 'Dumper\Runtime\EngineNameCodec.h'
$nameCodec = Read-ProjectFile 'Dumper\Runtime\EngineNameCodec.cpp'
$capabilities = Read-ProjectFile 'Dumper\Runtime\CoreCapabilities.h'
$shutdown = Read-ProjectFile 'Dumper\Runtime\ShutdownCoordinator.h'
$handleHeader = Read-ProjectFile 'Dumper\Runtime\ObjectHandle.h'
$handleImplementation = Read-ProjectFile 'Dumper\Runtime\ObjectHandle.cpp'
$identityLayout = Read-ProjectFile 'Dumper\Runtime\FUObjectItemLayout.cpp'
$identityContext = Read-ProjectFile 'Dumper\Runtime\ObjectIdentityContext.h'
$identitySourceHeader = Read-ProjectFile 'Dumper\Runtime\ObjectArrayIdentitySource.h'
$identitySourceImplementation = Read-ProjectFile 'Dumper\Runtime\ObjectArrayIdentitySource.cpp'
$identitySource = $identitySourceHeader + $identitySourceImplementation
$snapshotIdentitySource = Read-ProjectFile 'Dumper\Runtime\ObjectSnapshotIdentitySource.h'
$engineFacade = (Read-ProjectFile 'Dumper\Runtime\EngineFacade.h') +
	(Read-ProjectFile 'Dumper\Runtime\EngineFacade.cpp')
$engineSnapshotHeader = Read-ProjectFile 'Dumper\Runtime\EngineSnapshot.h'
$engineSnapshot = Read-ProjectFile 'Dumper\Runtime\EngineSnapshot.cpp'
$snapshotCaptureHeader = Read-ProjectFile 'Dumper\Runtime\EngineSnapshotCapture.h'
$snapshotCapture = Read-ProjectFile 'Dumper\Runtime\EngineSnapshotCapture.cpp'
$objectArrayHeader = Read-ProjectFile 'Dumper\Engine\Public\Unreal\ObjectArray.h'
$objectArrayImplementation = Read-ProjectFile 'Dumper\Engine\Private\Unreal\ObjectArray.cpp'
$objectArray = $objectArrayHeader + $objectArrayImplementation
$objectSnapshotSourceHeader = Read-ProjectFile 'Dumper\Runtime\ObjectArraySnapshotSource.h'
$objectSnapshotSource = Read-ProjectFile 'Dumper\Runtime\ObjectArraySnapshotSource.cpp'
$propertyCodecHeader = Read-ProjectFile 'Dumper\Runtime\PropertyCodec.h'
$propertyCodec = Read-ProjectFile 'Dumper\Runtime\PropertyCodec.cpp'
$reflectionLayoutHeader = Read-ProjectFile 'Dumper\Runtime\ReflectionLayout.h'
$reflectionLayout = Read-ProjectFile 'Dumper\Runtime\ReflectionLayout.cpp'
$reflectionCaptureHeader = Read-ProjectFile 'Dumper\Runtime\ReflectionLayoutCapture.h'
$reflectionCapture = Read-ProjectFile 'Dumper\Runtime\ReflectionLayoutCapture.cpp'
$reflectionSourceHeader = Read-ProjectFile 'Dumper\Runtime\ObjectSnapshotReflectionCandidateSource.h'
$reflectionSource = Read-ProjectFile 'Dumper\Runtime\ObjectSnapshotReflectionCandidateSource.cpp'
$typeSnapshotHeader = Read-ProjectFile 'Dumper\Runtime\TypeSnapshot.h'
$typeSnapshot = Read-ProjectFile 'Dumper\Runtime\TypeSnapshot.cpp'
$typeCaptureHeader = Read-ProjectFile 'Dumper\Runtime\TypeSnapshotCapture.h'
$typeCapture = Read-ProjectFile 'Dumper\Runtime\TypeSnapshotCapture.cpp'
$typeMetadataContext = Read-ProjectFile 'Dumper\Runtime\TypeMetadataContext.h'
$typeSourceHeader = Read-ProjectFile 'Dumper\Runtime\ObjectSnapshotTypeCandidateSource.h'
$typeSource = Read-ProjectFile 'Dumper\Runtime\ObjectSnapshotTypeCandidateSource.cpp'
$callbackBarrier = Read-ProjectFile 'Dumper\Runtime\CallbackBarrier.h'
$safeMemoryHeader = Read-ProjectFile 'Dumper\Runtime\SafeMemory.h'
$safeMemory = Read-ProjectFile 'Dumper\Runtime\SafeMemory.cpp'
$vtableHook = Read-ProjectFile 'Dumper\Runtime\VTableHook.cpp'
$gameThreadHeader = Read-ProjectFile 'Dumper\Runtime\GameThreadExecutor.h'
$gameThreadImplementation = Read-ProjectFile 'Dumper\Runtime\GameThreadExecutor.cpp'
$gameThread = $gameThreadHeader + $gameThreadImplementation
$frameSchedulerHeader = Read-ProjectFile 'Dumper\Runtime\GameThreadFrameScheduler.h'
$frameSchedulerImplementation = Read-ProjectFile 'Dumper\Runtime\GameThreadFrameScheduler.cpp'
$frameScheduler = $frameSchedulerHeader + $frameSchedulerImplementation
$commandHeader = Read-ProjectFile 'Dumper\Services\CoreCommandService.h'
$commandImplementation = Read-ProjectFile 'Dumper\Services\CoreCommandService.cpp'
$commandService = $commandHeader + $commandImplementation
$memoryApi = Read-ProjectFile 'Dumper\API\MemoryApi.cpp'
$objectsApi = Read-ProjectFile 'Dumper\API\ObjectsApi.cpp'
$hookApi = Read-ProjectFile 'Dumper\API\HookApi.cpp'
$callApi = Read-ProjectFile 'Dumper\API\CallApi.cpp'
$statusApi = Read-ProjectFile 'Dumper\API\StatusApi.cpp'
$main = Read-ProjectFile 'Dumper\Main.cpp'
$harness = Read-ProjectFile 'tests\core-harness\main.cpp'

foreach ($token in @('Created', 'Initializing', 'Ready', 'Failed', 'Stopping', 'Stopped',
        'RequestLease', 'CORE_STOPPING', 'WaitForRequests', 'ReadinessSatisfied',
        'SessionId', 'BeginInitialize(std::string sessionId)',
        'std::shared_ptr<const EngineContext>')) {
    Assert-Contains $runtime $token 'CoreRuntime state/ownership contract regressed.'
}
foreach ($token in @('BCryptGenRandom', 'BCRYPT_USE_SYSTEM_PREFERRED_RNG', 'core-')) {
    Assert-Contains $coreSession $token 'Secure Core session generation regressed.'
}

foreach ($token in @('Generation()', 'OffsetReport', 'Required engine offsets are not validated',
		'EngineNameProfile', 'NameProfile()', 'SetNameProfile',
		'std::shared_ptr<const EngineContext>')) {
    Assert-Contains $context $token 'Immutable EngineContext contract regressed.'
}

foreach ($token in @('gobjects', 'process_event.index', 'positive_member_offset',
		'count_within_capacity', 'validated_ini_override', 'fuobjectitem.serial_number',
		'CaptureNameProfile', 'runtime_name_storage_layout', 'NAME_STORAGE_LAYOUT_NOT_CAPTURED',
		'name_index_zero_decodes_none', 'NAME_STORAGE_SEMANTIC_VALIDATION_FAILED')) {
    Assert-Contains $capture $token 'Offset validation report regressed.'
}
foreach ($token in @('void InitRuntime();', 'void InitReflection();')) {
	Assert-Contains $offsetsHeader $token 'Runtime/reflection initialization boundary regressed.'
}
foreach ($token in @('void Off::InitRuntime()', 'void Off::InitReflection()',
		'Required runtime offset was not discovered')) {
	Assert-Contains $offsets $token 'Runtime/reflection offset initialization boundary regressed.'
}
foreach ($token in @('Off::InitRuntime();', 'Generator::InitEngineCore()')) {
	Assert-Contains $generator $token 'Baseline Generator initialization no longer uses the runtime-only path.'
}
foreach ($token in @('Off::Init();', 'PropertySizes::Init()', 'InitTextOffsets()',
		'InitGWorld()', 'InitGEngine()')) {
	Assert-NotContains $generator $token 'Baseline Generator initialization reintroduced optional reflection/world execution.'
}
foreach ($token in @('MemberOffset("ustruct.super_struct", Off::UStruct::SuperStruct, false)',
		'MemberOffset("property.array_dim", Off::Property::ArrayDim, false)',
		'MemberOffset("uclass.cast_flags", Off::UClass::CastFlags, true)')) {
	Assert-Contains $capture $token 'Optional reflection offsets became global Core requirements.'
}

foreach ($token in @('FNameStorageLayout', 'TryCaptureRuntimeLayout')) {
	Assert-Contains $nameArrayHeader $token 'Initialized name storage layout capture regressed.'
	Assert-Contains $nameArray $token 'Initialized name storage layout capture implementation regressed.'
}
foreach ($token in @('EngineNameError', 'IsEngineNameProfileLayoutValid', 'DecodeFName',
		'DecodeNamePool', 'DecodeChunkedArray', 'kMaxNameUnits', 'kMaxRedirectDepth',
		'MB_ERR_INVALID_CHARS', 'WC_ERR_INVALID_CHARS', 'ReadMemory', 'ReadValue')) {
	Assert-Contains ($nameCodecHeader + $nameCodec) $token 'Safe immutable name codec regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'NameArray::', 'FName::', ' FName(')) {
	Assert-NotContains $nameCodec $token 'Production name decoding bypassed its immutable profile boundary.'
}

foreach ($token in @('transport.named_pipe', 'PIPE_LISTENER_NOT_READY', 'objects.identity_source',
		'engine.names', 'NAME_STORAGE_LAYOUT_NOT_VALIDATED',
		'engine.reflection', 'REFLECTION_RUNTIME_NOT_PUBLISHED',
		'engine.property_codec', 'PROPERTY_CODEC_NOT_CONFIGURED',
		'functions.handles', 'FUNCTION_HANDLE_VALIDATION_NOT_READY',
        'GAME_THREAD_PUMP_NOT_OBSERVED', 'GAME_THREAD_PUMP_STALLED', 'RequiredReadyCapabilities')) {
    Assert-Contains $capabilities $token 'Capability dependency/readiness contract regressed.'
}

Assert-Contains $shutdown 'SafeToUnload' 'ShutdownCoordinator must report unload safety.'
Assert-Contains $gameThread 'PumpThreadStable' 'Game-thread pump identity diagnostics are missing.'
Assert-Contains $gameThread 'LastPumpTickMonotonicUs' 'Game-thread liveness diagnostics are missing.'
foreach ($token in @('GameThreadTaskTiming', 'TryGetTiming', 'PumpThreadWaitDenied',
        'releasedWork = std::move(task->Work)')) {
    Assert-Contains $gameThread $token 'Game-thread command timing/terminal ownership regressed.'
}
foreach ($token in @('IGameThreadFrameClient', 'AttachFrameClient', 'DetachFrameClient',
		'm_FrameClient.store(nullptr', 'm_FrameClientBarrier.BeginStopping',
		'm_FrameClientBarrier.WaitForDrain', 'm_DrainingClient',
		'if (!m_Executor.IsCurrentPumpThread())', 'kFrameWorkBudget')) {
	Assert-Contains $gameThread $token 'PostRender frame-client ownership/drain regressed.'
}
foreach ($token in @('kMaxClients = 8', 'kMaxFrameWorkBudget', 'kClientQuantum = 4',
		'kFrameTimeBudgetUs', 'AttachClient', 'DetachClient', 'm_NextSlot',
		'm_PumpOwned.test_and_set', 'slot.Client.store(nullptr', 'slot.Barrier.BeginStopping',
		'target->Barrier.WaitForDrain', 'ContractViolationCount', 'ZeroProgressCount',
		'TimeBudgetExhaustions')) {
	Assert-Contains $frameScheduler $token 'Bounded multi-client frame scheduler regressed.'
}

foreach ($token in @('status.inspect', 'status.engine', 'status.health',
        'objects.snapshot.page', 'objects.handle.issue', 'functions.handle.issue',
		'SNAPSHOT_GENERATION_MISMATCH', 'SNAPSHOT_CONTEXT_MISMATCH',
		'retired_snapshot_count', 'retired_captures',
		'kMaxSnapshotPageRecords = 128', 'std::upper_bound', 'SESSION_MISMATCH',
        'TryAcquireRequest', 'std::move(*lease)', 'm_GameThread.Enqueue',
        'onGameThreadQueued(ticket)', 'SerializeObjectHandle', 'SerializeFunctionHandle')) {
    Assert-Contains $commandService $token 'Transport-neutral Core command boundary regressed.'
}
Assert-NotContains $commandService 'HttpResponse' 'Core domain commands must not construct HTTP responses.'
Assert-NotContains $commandService 'HttpServer' 'Core domain commands must not depend on the legacy HTTP server.'

foreach ($token in @('CheckedAddressRange', 'ReadMemory', 'WriteMemory', 'CompareExchangePointer',
        'AllowExecutableWrite', 'FlushInstructionCache')) {
    Assert-Contains $safeMemoryHeader $token 'SafeMemory public contract regressed.'
}

foreach ($token in @('SessionId', 'ContextGeneration', 'SerialNumber', 'Address',
        'ClassFingerprint', 'FunctionHandle', 'IHandleIdentitySource',
		'ContextGeneration() const noexcept', 'IsCurrentExecutionThreadValid() const noexcept',
		'ValidateObject', 'ValidateFunction')) {
    Assert-Contains $handleHeader $token 'Stable object/function handle contract regressed.'
}
foreach ($token in @('HANDLE_SESSION_MISMATCH', 'HANDLE_CONTEXT_GENERATION_MISMATCH',
        'HANDLE_SERIAL_MISMATCH', 'FUNCTION_HANDLE_OWNER_MISMATCH',
		'HANDLE_EXECUTION_THREAD_INVALID', 'm_Source.ContextGeneration() == m_ContextGeneration',
        'IsCanonicalFunctionIdentityPath', 'CompareIdentity(handle.Function',
        'CompareIdentity(handle.Owner')) {
    Assert-Contains $handleImplementation $token 'Execution-point handle validation regressed.'
}
foreach ($token in @('epic_fuobjectitem_64_v1', 'exact_epic_object_offset',
        'supported_epic_item_size', 'positive_serial_witness',
        'FUOBJECTITEM_POSITIVE_SERIAL_NOT_OBSERVED')) {
    Assert-Contains $identityLayout $token 'FUObjectItem serial profile validation regressed.'
}
foreach ($token in @('CaptureObjectIdentityContext', 'CanIssueObjectHandles',
		'CanIssueFunctionHandles', 'FUObjectItemSerial', 'FNameComparisonIndex',
		'FunctionExec')) {
	Assert-Contains $identityContext $token 'Immutable object-identity offset context regressed.'
}
foreach ($token in @('TryReadIdentityCandidate', 'objectFirst != objectSecond', 'internalIndex != Index',
		'FUObjectItemSerialNumberOffset', 'count > capacity', 'Index >= count',
		'internalIndexOffset > (std::numeric_limits<uintptr_t>::max)() - objectAddress')) {
    Assert-Contains $objectArray $token 'Production FUObjectItem identity reads regressed.'
}
foreach ($token in @('EFUObjectItemReadResult', 'Captured', 'Empty', 'Failed',
		'TryReadIdentitySlot', 'TryGetCounts')) {
	Assert-Contains $objectArray $token 'Typed production object-slot reads regressed.'
}
foreach ($token in @('IObjectSnapshotIdentitySource', 'CanReadObjectSlots',
		'TryGetObjectCount', 'ObjectSnapshotSlotReadResult')) {
	Assert-Contains $snapshotIdentitySource $token 'Snapshot identity-source boundary regressed.'
	Assert-Contains $identitySource $token 'Production identity source no longer implements snapshot slot reads.'
}
foreach ($token in @('IsCurrentExecutionThreadValid', 'executor.IsCurrentPumpThread()',
        'TryReadObjectCore', 'TryReadCanonicalFNameToken', 'TryBuildCanonicalFunctionPath',
		'm_Offsets.ObjectClass', 'm_Offsets.FunctionExec',
		'finalPath != fullPath', 'SignatureFingerprint')) {
    Assert-Contains $identitySource $token 'Production object/function identity source regressed.'
}
Assert-NotContains $identitySource 'Off::' 'Production identity validation must use its immutable context, not mutable offset globals.'
foreach ($token in @('TryValidateLiveAddress', 'TryReadNode', 'TryReadKind', 'TryBuildPath',
		'TryBuildRecord', 'SameRecord(first, second)', 'EngineObjectKind::Package',
		'ValidateObjectHandle', 'ObjectSnapshotSlotReadResult::Empty', 'ReadValue')) {
	Assert-Contains ($objectSnapshotSourceHeader + $objectSnapshotSource) $token 'Production object snapshot metadata source regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'NameArray::', 'ObjectArray::', 'UEObject ', 'UEClass ')) {
	Assert-NotContains $objectSnapshotSource $token 'Production snapshot metadata bypassed its immutable source/SafeMemory boundary.'
}
foreach ($token in @('ObjectHandleService', 'EngineNameCodec', 'Names() const noexcept',
		'EngineSnapshotStore', 'EngineSnapshotCapture',
		'PropertyCodec', 'ConfigureReflectionLayout', 'ConfigurePropertyCodec',
		'Reflection() const noexcept',
		'Properties() const noexcept', 'ReflectionRuntimeSnapshot',
		'std::atomic<std::shared_ptr<const ReflectionRuntimeSnapshot>>', 'm_ReflectionMutex',
		'TypeSnapshotStore', 'PublishTypeSnapshot', 'Types() const noexcept',
		'ConfigureSnapshotCapture', 'IssueObjectHandle', 'ValidateFunctionHandle',
		'std::shared_ptr<const EngineContext>')) {
	Assert-Contains $engineFacade $token 'EngineFacade ownership boundary regressed.'
}
foreach ($token in @('PropertyValueState', 'Ok', 'Empty', 'Unsupported', 'Unavailable', 'Error',
		'PropertyCodecProfile', 'ReflectionLayoutFingerprint', 'IPropertyReferenceResolver', 'PropertyDecodeLimits',
		'SessionId() const noexcept', 'ContextGeneration() const noexcept',
		'MaxContainerElements', 'MaxTotalNodes', 'MaxReadableContainerBytes',
		'PropertyDescriptor', 'PropertyObjectReference')) {
	Assert-Contains $propertyCodecHeader $token 'Property codec public contract regressed.'
}
foreach ($token in @('ReadMemory', 'ReadValue', 'ValidateReadableMemory',
		'PROPERTY_CONTAINER_HEADER_INVALID', 'PROPERTY_CONTAINER_SIZE_OVERFLOW',
		'PROPERTY_VALUE_CHANGED_DURING_READ', 'PROPERTY_RECURSION_CYCLE',
		'PROPERTY_REFERENCE_RESULT_INVALID', 'IsStableObjectHandle',
		'IsReferenceResolverConfigured',
		'PROPERTY_DELEGATE_UNSUPPORTED', 'PROPERTY_REFERENCE_RESOLVER_UNAVAILABLE',
		'PROPERTY_STRING_ENCODING_INVALID', 'IsPropertyCodecProfileValid',
		'DecodeSparse', 'DecodeSoftObject', 'DecodeStruct', 'DecodeArray')) {
	Assert-Contains $propertyCodec $token 'Bounded property codec implementation regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'ObjectArray::', '#include "Unreal/',
		'Platform::IsBadReadPtr', 'reinterpret_cast<const TArray')) {
	Assert-NotContains $propertyCodec $token 'Property codec bypassed its immutable descriptor/SafeMemory boundary.'
}
foreach ($token in @('ReflectionPropertySystem', 'ReflectionFieldCandidate',
		'ReflectionFieldWitness', 'ReflectionLayoutValidationResult',
		'ValidateReflectionLayout', 'IsReflectionLayoutValid',
		'ReflectionRuntimeSnapshot', 'IsLayoutConfigured',
		'IsPropertyCodecConfigured', 'ValidatedOnThreadId',
		'StructPropertiesSize', 'StructMinAlignment',
		'ReflectionLayoutFingerprint', 'WitnessIds', 'FieldOverlap')) {
	Assert-Contains ($reflectionLayoutHeader + $reflectionLayout) $token 'Reflection layout witness boundary regressed.'
}
foreach ($token in @('ReadMemory', 'ReadValue', 'ValidateReadableMemory',
		'DecodeFName', 'GetCurrentThreadId', 'LayoutFingerprint')) {
	Assert-Contains $reflectionLayout $token 'Reflection semantic validation regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'ObjectArray::', '#include "Unreal/',
		'Platform::IsBadReadPtr')) {
	Assert-NotContains $reflectionLayout $token 'Reflection layout validation bypassed SafeMemory or immutable profiles.'
}
foreach ($token in @('IReflectionCandidateSource', 'kMaxSourceSteps = 4096',
		'CaptureNext()', 'ValidateDependencies()', 'm_Source.Cancel()',
		'SourceContractViolation', 'StopAndDrain')) {
	Assert-Contains ($reflectionCaptureHeader + $reflectionCapture) $token 'Bounded reflection capture ownership regressed.'
}
foreach ($token in @('ObjectSnapshotReflectionCandidateSource', 'Prepare() noexcept',
		'ReleasePreparedPlan', 'PropertySystemMismatch', 'kOffsetCandidatesPerStep = 1',
		'kMaximumFieldChainDepth = 512', 'ReadStable', 'ValidateObjectHandle',
		'ValidateRelations', 'BuildEvidence', 'ValidateDependencies',
		'/Script/CoreUObject.Guid', '/Script/Engine.GameViewportClient')) {
	Assert-Contains ($reflectionSourceHeader + $reflectionSource) $token 'Production reflection source evidence boundary regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'ObjectArray::', 'NameArray::',
		'#include "Unreal/', 'UEObject ', 'UEStruct ', 'UEProperty ')) {
	Assert-NotContains $reflectionSource $token 'Production reflection discovery reached mutable legacy wrappers or offsets.'
}
foreach ($token in @('TypeSnapshotCandidate', 'TypeSnapshotStore',
		'ReflectedMemberState', 'ClassDefaultObjectState', 'TypeMemberScope',
		'QueryTypeProperties', 'QueryTypeFunctions', 'DescriptorCycle',
		'FunctionCoverageMismatch', 'HierarchyDepthExceeded',
		'std::shared_ptr<const EngineSnapshot>', 'std::shared_ptr<const ReflectionLayout>',
		'std::atomic<std::shared_ptr<const TypeSnapshot>>')) {
	Assert-Contains ($typeSnapshotHeader + $typeSnapshot) $token 'Immutable type snapshot boundary regressed.'
}
foreach ($token in @('CloneDescriptor', 'context.Visiting', 'SameHandle',
		'FunctionCoverageMismatch', 'defaultObject->ClassPath != type.FullPath',
		'property.Offset < parent->PropertiesSize', 'TryDeriveParameterDirection',
		'kPropertyFlagParm', 'm_ValidationFingerprint')) {
	Assert-Contains $typeSnapshot $token 'Type snapshot semantic validation regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'ObjectArray::', '#include "Unreal/',
		'UEObject ', 'UEStruct ', 'UEProperty ')) {
	Assert-NotContains $typeSnapshot $token 'Type snapshot validation reached legacy mutable reflection wrappers.'
}
foreach ($token in @('ITypeSnapshotSource', 'TypeSnapshotSourceRecord',
		'TypeSnapshotTypeBegin', 'TypeSnapshotPropertyRecord',
		'TypeSnapshotFunctionBegin', 'TypeSnapshotParameterRecord',
		'TypeSnapshotFunctionEnd', 'TypeSnapshotEnumEntryRecord', 'TypeSnapshotTypeEnd',
		'IGameThreadFrameClient', 'BeginValidation()', 'ValidateNext()',
		'ValidateDependencies()', 'Ready', 'Publishing', 'PublishReady',
		'kMaxRetiredCandidates = 4', 'ReclaimRetired', 'StopAndDrain',
		'CallbackBarrier')) {
	Assert-Contains ($typeCaptureHeader + $typeCapture) $token 'Budgeted type snapshot capture contract regressed.'
}
foreach ($token in @('m_PumpOwned.test_and_set', 'm_PublishOwned.test_and_set',
		'begun.Reflection != currentReflection',
		'm_Engine.Snapshots().Current() != m_ObjectDependency',
		'reflection != m_ReflectionDependency', 'CandidateAssemblyComplete',
		'm_Engine.PublishTypeSnapshot', 'RetireCandidate',
		'm_WorkBarrier.WaitForDrain', 'TypeSnapshotSourceError::AllocationFailed',
		'TypeSnapshotPublishError::WorkerThreadRequired')) {
	Assert-Contains $typeCapture $token 'Type snapshot capture ownership, dependency, or worker handoff regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'ObjectArray::', 'NameArray::',
		'#include "Unreal/', 'UEObject ', 'UEStruct ', 'UEProperty ')) {
	Assert-NotContains $typeCapture $token 'Generic type snapshot capture reached mutable legacy reflection state.'
}
foreach ($token in @('TypeMetadataContext', 'ClassDefaultObject',
		'FunctionFlags', 'FunctionExec', 'CaptureTypeMetadataContext')) {
	Assert-Contains $typeMetadataContext $token 'Frozen type metadata context regressed.'
}
foreach ($token in @('ObjectSnapshotTypeCandidateSource', 'Prepare() noexcept',
		'ReleasePreparedPlan', 'PreparedPlan', 'TypeIndexByAddress',
		"kMaximumFieldChainDepth = 65'536", 'ReadStable', 'DecodeFNameStable',
		'IssueFunctionHandle', 'ValidateObjectHandle', 'BeginValidation',
		'ValidateEvidence', 'PROPERTY_DESCRIPTOR_NOT_CAPTURED',
		'ENUM_LAYOUT_NOT_CAPTURED', 'FUNCTION_IMPLEMENTATION_NOT_WITNESSED',
		'FUNCTION_BYTECODE_NOT_CAPTURED', 'kFunctionFlagNative',
		'ValidateDependencies')) {
	Assert-Contains ($typeSourceHeader + $typeSource) $token 'Production type source evidence boundary regressed.'
}
foreach ($token in @('Off::', 'Settings::', 'ObjectArray::', 'NameArray::',
		'#include "Unreal/', 'UEObject ', 'UEStruct ', 'UEProperty ')) {
	Assert-NotContains $typeSource $token 'Production type source reached mutable legacy wrappers or offsets.'
}
foreach ($token in @('ConfigureTypeSnapshotCapture',
		'friend class TypeSnapshotCapture', 'std::unique_ptr<TypeSnapshotCapture>',
		'm_TypeCapture->StopAndDrain')) {
	Assert-Contains $engineFacade $token 'EngineFacade no longer uniquely owns and drains type capture.'
}
foreach ($token in @('std::shared_ptr<const ReflectionRuntimeSnapshot> Reflection',
		'probes.Reflection->IsLayoutConfigured(context.Generation())',
		'probes.Reflection->IsPropertyCodecConfigured(context.Generation())',
		'probes.Types->ObjectSnapshotGeneration() == probes.ObjectSnapshotGeneration',
		'probes.Types->IsConfigured(context.Generation())', 'engine.type_snapshot',
		'REFLECTION_RUNTIME_NOT_PUBLISHED', 'REFLECTION_RUNTIME_INVALID')) {
	Assert-Contains $capabilities $token 'Reflection capability no longer derives from the immutable runtime bundle.'
}
foreach ($token in @('ReflectionLayoutValidated', 'PropertyCodecEnabled')) {
	Assert-NotContains $capabilities $token 'A forgeable reflection boolean probe reopened the capability path.'
}
foreach ($token in @('DriveReflectionDiscovery', 'ReflectionCaptureBlocksSnapshotRefresh',
		'g_ReflectionFrameClientAttached', 'DetachReflectionFrameClient',
		'ObjectSnapshotReflectionCandidateSource',
		'shutdown.AddStage("reflection_frame_client"')) {
	Assert-Contains $main $token 'Main no longer owns the production reflection/snapshot exclusion lifecycle.'
}
foreach ($token in @('DriveTypeDiscovery', 'TypeCaptureBlocksSnapshotRefresh',
		'g_TypeFrameClientAttached', 'DetachTypeFrameClient',
		'ObjectSnapshotTypeCandidateSource', 'PublishReady',
		'shutdown.AddStage("type_frame_client"')) {
	Assert-Contains $main $token 'Main no longer owns the production type/snapshot exclusion lifecycle.'
}
foreach ($token in @('SerializeReflectionDiagnostics', 'preparation_error_code',
		'validation_error_code', 'prepared_snapshot_generation', 'layout_fingerprint')) {
	Assert-Contains $commandService $token 'Reflection preparation/capture diagnostics are no longer observable.'
}
foreach ($token in @('SerializeTypeSnapshotDiagnostics', 'type_snapshot',
		'publish_error_code', 'captured_types', 'captured_functions',
		'captured_members', 'validation_steps', 'retired_candidates',
		'prepared_functions', 'captured_evidence')) {
	Assert-Contains $commandService $token 'Type snapshot capture/publication diagnostics are no longer observable.'
}
foreach ($token in @('EngineSnapshotObject', 'SessionId', 'ContextGeneration', 'Generation',
		'SourceObjectCount', 'SkippedSlots', 'std::deque<EngineSnapshotObject>',
		'ValidatedEngineSnapshot', 'PublishValidated', 'kMaxRetiredSnapshots',
		'ReclaimRetired', 'std::atomic<std::shared_ptr<const EngineSnapshot>>')) {
	Assert-Contains $engineSnapshotHeader $token 'Immutable EngineSnapshot contract regressed.'
}
foreach ($token in @('SNAPSHOT_GENERATION_NOT_MONOTONIC', 'SNAPSHOT_RECORDS_NOT_ORDERED',
		'SNAPSHOT_RETIREMENT_BACKPRESSURE', 'IsValidHandleEnvelope', 'IsValidMetadata',
		'ValidateRecordForPublication', 'm_Current.exchange', 'm_RetiredSnapshots',
		'm_RejectedSnapshot', 'm_Current.load')) {
	Assert-Contains $engineSnapshot $token 'Atomic EngineSnapshot publication regressed.'
}
foreach ($token in @('IEngineSnapshotSource', 'IGameThreadFrameClient', 'kFramePumpBudget',
		'kDefaultPumpBudget', 'kMaxPumpBudget', 'PumpFrame',
		'Capturing', 'Validating', 'Publishing', 'StopAndDrain', 'CallbackBarrier')) {
	Assert-Contains $snapshotCaptureHeader $token 'Incremental snapshot capture contract regressed.'
}
foreach ($token in @('m_PumpOwned.test_and_set', 'SnapshotSlotReadResult::Empty',
		'ValidateSlot(index, expected)', 'ValidateRecordForPublication',
		'ValidatedEngineSnapshot', 'm_Store.PublishValidated', 'RetireWorkingCapture',
		'ReclaimRetired', 'kMaxRetiredCaptures',
		'published.Error == SnapshotPublishError::RetirementBackpressure',
		'SnapshotCaptureError::SourceValidationFailed', 'SnapshotCaptureError::SourceCountChanged',
		'publishObjectCount != m_Working->SourceObjectCount', 'm_PumpBarrier.WaitForDrain')) {
	Assert-Contains $snapshotCapture $token 'Budgeted snapshot capture implementation regressed.'
}
Assert-NotContains $snapshotCapture 'ObjectArray::' 'Generic snapshot scheduling must not bypass its source boundary.'
Assert-NotContains $snapshotCapture 'Off::' 'Generic snapshot scheduling must not read mutable engine offsets.'
foreach ($token in @('CALL_HANDLE_REQUIRED', 'SESSION_SERIAL_OBJECT_AND_FUNCTION_HANDLES_REQUIRED',
        'server.Post("/api/v1/call/function"',
        'server.Post("/api/v1/call/static"',
        'server.Post("/api/v1/call/batch"')) {
    Assert-Contains $callApi $token 'Legacy index-only call path became reachable.'
}
$callHandleGateCount = [regex]::Matches($callApi, 'return CallHandleRequired\(\);').Count
if ($callHandleGateCount -ne 3) {
    throw "Every legacy call route must return through the stable-handle gate; found $callHandleGateCount gates."
}
foreach ($token in @('VirtualQuery', 'CopyWithSeh', 'CompareExchangePointerWithSeh',
        'RestoreProtections', 'ExecutableWriteDenied', 'InstructionCacheFlushRequired')) {
    Assert-Contains $safeMemory $token 'SafeMemory implementation contract regressed.'
}
foreach ($token in @('std::from_chars', 'Too many offsets (max 64)', 'POINTER_CHAIN_OVERFLOW',
        'std::vector<std::int64_t>', 'Runtime::WriteMemory')) {
    Assert-Contains $memoryApi $token 'Memory API validation contract regressed.'
}
Assert-NotContains $memoryApi 'std::stoull' 'Memory API must fully parse addresses without exception-based partial conversion.'
Assert-Contains $objectsApi 'OBJECT_PROPERTY_WRITE_DISABLED' 'Unsafe raw UObject property writes became reachable.'
Assert-Contains $hookApi 'VTableHookToken::Install' 'Hook patching bypasses the RAII owner.'
Assert-Contains $vtableHook 'CompareExchangePointer' 'VTable Hook patching bypasses atomic SafeMemory.'
Assert-Contains $callbackBarrier 'WaitForDrain' 'Hook callback quiescence barrier is missing.'

foreach ($apiFile in Get-ChildItem -LiteralPath (Join-Path $root 'Dumper\API') -Filter '*.cpp') {
    $apiSource = Get-Content -LiteralPath $apiFile.FullName -Raw -Encoding UTF8
    Assert-NotContains $apiSource 'VirtualProtect' "API module $($apiFile.Name) bypasses SafeMemory."
}

foreach ($token in @('CaptureEngineContext', 'RefreshRuntimeCapabilities', 'ShutdownCoordinator',
		'ObjectArraySnapshotSource', 'ConfigureSnapshotCapture', 'GetGameThreadFrameScheduler',
		'AttachFrameClient', 'AttachClient', 'RequestCapture', 'DetachClient',
		'DetachFrameClient', 'ReclaimSnapshotStorage', 'snapshot_frame_client', 'frame_scheduler',
		'BeginStopping', 'MarkStopped')) {
    Assert-Contains $main $token 'Main does not use the runtime ownership path.'
}
Assert-NotContains $main 'Generator::InitInternal()' 'Baseline Core startup eagerly builds the mutable Generator type index.'

foreach ($token in @('liveness', 'readiness', 'offset_reports', 'capabilities', 'context_generation',
		'last_tick_monotonic_us', 'queue_depth', 'object_snapshot', 'name_profile',
		'SerializeNameProfile')) {
    Assert-Contains $commandService $token 'Core status command does not expose truthful runtime state.'
}
Assert-NotContains $statusApi 'Off::' 'Status handlers must read the immutable EngineContext, not raw offset globals.'
Assert-NotContains $statusApi 'Settings::' 'Status handlers must read the immutable EngineContext, not mutable settings globals.'
Assert-NotContains $statusApi 'ObjectArray::' 'Status handlers must not query the live object array from HTTP workers.'
foreach ($token in @('service->Execute', 'status.inspect', 'status.engine', 'status.health')) {
    Assert-Contains $statusApi $token 'Legacy status adapter bypassed the Core command service.'
}
Assert-NotContains $statusApi 'CoreRuntimeSnapshot' 'Status HTTP adapter must not duplicate runtime business logic.'

foreach ($token in @('TestEngineContextAndCapabilities', 'TestCoreRuntimeStateAndShutdown',
        'TestCoreSessionIdentity',
		'TestEngineNameCodec', 'Invalid UTF-8 FName entry was accepted',
		'TestPropertyCodec', 'Property value states are not explicit and stable',
		'TestReflectionLayout', 'A partial reflection field set was accepted',
		'TestObjectSnapshotReflectionCandidateSource',
		'TestFPropertySnapshotReflectionCandidateSource',
		'Production reflection source did not publish the witnessed immutable layout',
		'FProperty reflection source did not publish the witnessed layout',
		'A non-power-of-two UStruct minimum alignment was accepted',
		'Layout-only reflection publication was not immutable or fingerprint-bound',
		'A validated layout did not open reflection independently from property decoding',
		'A partial type snapshot was published',
		'A type snapshot omitted a live direct function',
		'A class accepted another class''s default object',
		'A cyclic class hierarchy was published',
		'A cyclic mutable property descriptor was frozen into a type snapshot',
		'An unknown reflected member state was published',
		'Parameter direction disagreed with its reflected flags',
		'Direct and inherited type-member semantics were not explicit and stable',
		'A type snapshot survived an object snapshot generation change',
		'EngineFacade did not uniquely own its type snapshot producer',
		'Budgeted type capture published partial data or misreported exact work',
		'Worker publication did not atomically publish the complete type generation',
		'Type snapshot heavy publication ran on the witnessed game thread',
		'A changed immutable dependency reached type snapshot publication',
		'Type validation continued after execution-thread invalidation',
		'A no-progress type source escaped bounded retirement',
		'Type capture retirement backpressure lost or frame-thread-destroyed a candidate',
		'An incomplete type/function record stream reached immutable publication',
		'Reflection validation was published from a different execution thread',
		'FString was not copied and converted through the bounded UTF-16 codec',
		'Validated FText layout returned an unresolved placeholder',
		'Object property returned a raw address instead of a stable handle',
		'Reference resolver success bypassed the stable-handle envelope checks',
		'Reference resolver success returned a handle for a different address',
		'Reference resolver returned a handle from a different context generation',
		'Soft object path was guessed as weak index/serial or decoded out of order',
		'Recursive property descriptor/address pair bypassed the cycle guard',
		'Recursive array descriptor/address pair bypassed the generic cycle guard',
		'Array with Num greater than Max was accepted',
		'Total property node budget did not stop and truncate the output value tree',
		'Sparse container item wrappers bypassed the total property node budget',
		'Sparse map key/value pairs were not decoded through bounded descriptors',
		'Overlapping sparse map key/value ranges were accepted',
		'Overlapping layout fields configured a partially usable property codec',
		'Unimplemented delegate codec was reported as a successful value',
		'NamePool redirect cycle was not rejected',
		'Chunked name-array entry identity mismatch was accepted',
		'Name codec did not convert an inaccessible storage read into a stable error',
		'Status domain command did not serialize the immutable runtime/name profile',
		'TestProductionSnapshotMetadataSource',
		'Production snapshot source confused an empty slot with a read failure',
		'Production snapshot metadata path or kind is incorrect',
		'Production snapshot validation ignored slot recycling',
		'TestEngineFacadeAndImmutableSnapshots', 'Snapshot reader observed a torn generation',
		'Incomplete snapshot metadata was published as usable data',
		'TestIncrementalSnapshotCapture', 'Snapshot pump exceeded its per-frame work budget',
		'A slot mutation between capture and validation was published',
		'Object-count mutation was published as a complete snapshot',
		'Object-count mutation during publication replaced the complete snapshot',
		'Snapshot producer deferred malformed-record validation to the final publication frame',
		'Failed snapshot working sets were destroyed on the frame thread instead of retired',
		'Atomic snapshot replacement did not defer prior-generation reclamation',
		'Snapshot failure retirement exceeded its fixed capacity without backpressure',
		'Validated publication exceeded retirement capacity without explicit backpressure',
		'TestGameThreadFrameSchedulerBudgetFairnessAndDrain',
		'Frame scheduler did not distribute the first round fairly',
		'Frame scheduler detach did not withdraw and retain an in-flight owner',
		'PostRender dispatched frame work after detecting a pump-thread mismatch',
		'Concurrent snapshot pumps accessed one mutable working generation',
		'Snapshot source exception escaped the guarded pump boundary',
		'Snapshot producer crossed an identity-source context generation',
		'Snapshot shutdown ignored an in-flight pump',
		'TestCoreDomainCommandsAndHandleExecution', 'Handle command accepted transport-supplied identity fields',
		'Snapshot first page did not preserve immutable generation and exact totals',
		'Snapshot continuation page skipped or repeated an object index',
		'Snapshot cursor silently crossed an immutable generation boundary',
		'Worker-safe snapshot paging entered the game-thread command queue',
		'Missing function metadata did not disable only function handles',
		'Function handle command ignored its dedicated capability',
        'Domain command ticket did not cancel queued work',
        'TestStableObjectAndFunctionHandles', 'Object handle crossed a session boundary',
        'Recycled object slot retained a valid handle', 'Function handle ignored owner recycling',
        'Non-canonical display path became a function execution identity',
        'TestFUObjectItemIdentityLayout', 'Custom FUObjectItem object offset was guessed',
        'Zero-only serial candidate was accepted',
        'TestHookOwnershipAndCallbackDrain', 'Failed VTable restore discarded hook ownership',
        'TestGenericGameThreadWorkAndCancellation', 'explicitly cancelled',
		'TestPostRenderFrameClientOwnershipAndDrain',
		'PostRender frame-client detach ignored an in-flight callback',
		'Post-stop callback was allowed to run owned frame-client work',
        'Post-stop callback was allowed to run owned work',
        'TestSafeMemory', 'ExecutableWriteDenied', 'InstructionCacheFlushRequired',
		'CoreRuntime became Ready without its pipe listener', 'Required capability loss left readiness true',
		'Unvalidated optional reflection metadata leaked into a domain capability',
        'Shutdown coordinator ran twice')) {
    Assert-Contains $harness $token 'CoreRuntime harness coverage regressed.'
}

$payloadSchema = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\schema\payload.schema.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$objectHandleFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-handle.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$functionHandleFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\function-handle.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$objectHandleRequestFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-handle-request.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$objectHandleResponseFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-handle-response.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$snapshotPageRequestFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-snapshot-page-request.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$snapshotPageResponseFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-snapshot-page-response.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($null -eq $payloadSchema.'$defs'.objectHandle -or $null -eq $payloadSchema.'$defs'.functionHandle -or
        $null -eq $payloadSchema.'$defs'.handleIssueData -or $null -eq $payloadSchema.'$defs'.emptyCommandData -or
		$null -eq $payloadSchema.'$defs'.snapshotCursor -or $null -eq $payloadSchema.'$defs'.snapshotPageData -or
		$null -eq $payloadSchema.'$defs'.snapshotRecord -or $null -eq $payloadSchema.'$defs'.snapshotPageResult) {
	throw 'IPC payload schema does not define stable handles and snapshot paging.'
}
if ($objectHandleFixture.serial -le 0 -or $objectHandleFixture.context_generation -le 0) {
    throw 'Object handle fixture lacks a positive serial or context generation.'
}
if ($functionHandleFixture.function.session_id -ne $functionHandleFixture.owner.session_id -or
        $functionHandleFixture.function.context_generation -ne $functionHandleFixture.owner.context_generation) {
    throw 'Function handle fixture crosses a session or context generation.'
}
if ($functionHandleFixture.full_path -notmatch '^Function fname:[0-9a-f]+:[0-9]+(?:\.fname:[0-9a-f]+:[0-9]+)+$') {
    throw 'Function handle fixture does not use the canonical raw-FName identity path.'
}
if ($objectHandleRequestFixture.operation -ne 'objects.handle.issue' -or
        $objectHandleRequestFixture.data.PSObject.Properties.Name.Count -ne 1 -or
        $objectHandleRequestFixture.data.index -ne $objectHandleResponseFixture.data.index -or
        $objectHandleRequestFixture.session_id -ne $objectHandleResponseFixture.session_id) {
    throw 'Object handle command fixtures do not preserve strict discovery input/session identity.'
}
if ($snapshotPageRequestFixture.operation -ne 'objects.snapshot.page' -or
		$snapshotPageRequestFixture.data.PSObject.Properties.Name.Count -ne 2 -or
		$null -ne $snapshotPageRequestFixture.data.cursor -or
		$snapshotPageRequestFixture.data.limit -lt 1 -or
		$snapshotPageRequestFixture.data.limit -gt 128) {
	throw 'Snapshot page request fixture does not preserve the strict null-cursor/limit contract.'
}
if (-not $snapshotPageResponseFixture.ok -or
		$snapshotPageResponseFixture.session_id -ne $snapshotPageRequestFixture.session_id -or
		$snapshotPageResponseFixture.data.items.Count -ne $snapshotPageRequestFixture.data.limit -or
		$snapshotPageResponseFixture.data.source_object_count -ne
			($snapshotPageResponseFixture.data.record_count + $snapshotPageResponseFixture.data.skipped_slots) -or
		-not $snapshotPageResponseFixture.data.has_more -or
		$snapshotPageResponseFixture.data.next_cursor.generation -ne $snapshotPageResponseFixture.data.generation -or
		$snapshotPageResponseFixture.data.next_cursor.after_index -ne
			$snapshotPageResponseFixture.data.items[-1].handle.index) {
	throw 'Snapshot page response fixture lost exact totals or its generation-bound cursor.'
}
$previousSnapshotIndex = -1
foreach ($record in $snapshotPageResponseFixture.data.items) {
	if ($record.handle.session_id -ne $snapshotPageResponseFixture.session_id -or
			$record.handle.context_generation -ne $snapshotPageResponseFixture.data.context_generation -or
			$record.handle.index -le $previousSnapshotIndex) {
		throw 'Snapshot page response fixture contains a crossed envelope or unordered record.'
	}
	$previousSnapshotIndex = $record.handle.index
}

Write-Host 'Core runtime contract verified: immutable context, stable handles, generation-bound snapshot paging, capability readiness, SafeMemory, request drain, and coordinated shutdown.'
