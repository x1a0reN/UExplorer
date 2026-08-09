use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
#[cfg(windows)]
use std::time::Duration;
use tauri::command;
use tauri::ipc::Channel;

pub mod ipc;
pub mod session;

#[cfg(windows)]
use crate::ipc::named_pipe_client::CoreRpcClientError;
use crate::session::event_bridge::{EventBridgeDiagnostics, EventBridgeManager, EventSink};
use crate::session::event_hub::{EventFilter, HostEvent};
#[cfg(windows)]
use crate::session::session_manager::{
    ManagedSessionDiagnostics, SessionManager, SessionManagerDiagnostics, SessionManagerError,
    SessionPhase, TargetProcessIdentity,
};
#[cfg(windows)]
use std::collections::BTreeSet;
#[cfg(windows)]
use std::ffi::c_void;
#[cfg(windows)]
use std::os::windows::ffi::OsStrExt;
#[cfg(windows)]
use std::sync::{Arc, Mutex};
#[cfg(windows)]
use tauri::State;
#[cfg(windows)]
use windows::core::{Error as WindowsError, HRESULT, PCSTR, PCWSTR, PWSTR};
#[cfg(windows)]
use windows::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_NO_MORE_FILES, FILETIME, HANDLE, WAIT_FAILED, WAIT_OBJECT_0,
    WAIT_TIMEOUT,
};
#[cfg(windows)]
use windows::Win32::System::Diagnostics::Debug::WriteProcessMemory;
#[cfg(windows)]
use windows::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Module32FirstW, Module32NextW, Process32FirstW, Process32NextW,
    MODULEENTRY32W, PROCESSENTRY32W, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32, TH32CS_SNAPPROCESS,
};
#[cfg(windows)]
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
#[cfg(windows)]
use windows::Win32::System::Memory::{
    VirtualAllocEx, VirtualFreeEx, MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE,
};
#[cfg(windows)]
use windows::Win32::System::SystemInformation::{
    IMAGE_FILE_MACHINE, IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_ARM64,
    IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_UNKNOWN,
};
#[cfg(windows)]
use windows::Win32::System::Threading::{
    CreateRemoteThread, GetExitCodeThread, GetProcessTimes, IsWow64Process2, OpenProcess,
    QueryFullProcessImageNameW, WaitForSingleObject, INFINITE, PROCESS_CREATE_THREAD,
    PROCESS_NAME_WIN32, PROCESS_QUERY_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION,
    PROCESS_VM_OPERATION, PROCESS_VM_READ, PROCESS_VM_WRITE,
};

// Process info structure
#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct ProcessInfo {
    pub pid: u32,
    pub name: String,
    pub path: String,
    pub start_time_100ns: String,
    pub architecture: String,
    pub candidate_reasons: Vec<String>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum InjectionStatus {
    Ready,
    Failed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum DllLoadState {
    NotAttempted,
    Loaded,
    AlreadyLoaded,
    Indeterminate,
    Failed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PipeConnectionState {
    NotAttempted,
    Connected,
    Failed,
    Rejected,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CoreReadinessState {
    NotChecked,
    Ready,
    Failed,
}

#[derive(Debug, Serialize)]
pub struct InjectionResult {
    pub success: bool,
    pub status: InjectionStatus,
    pub stage: String,
    pub code: String,
    pub message: String,
    pub dll: DllLoadState,
    pub pipe: PipeConnectionState,
    pub core: CoreReadinessState,
    #[cfg(windows)]
    pub session: Option<ManagedSessionDiagnostics>,
}

fn injection_failure(
    stage: impl Into<String>,
    code: impl Into<String>,
    detail: impl Into<String>,
    dll: DllLoadState,
    pipe: PipeConnectionState,
    core: CoreReadinessState,
) -> InjectionResult {
    let code = code.into();
    InjectionResult {
        success: false,
        status: InjectionStatus::Failed,
        stage: stage.into(),
        message: format!("{code}: {}", detail.into()),
        code,
        dll,
        pipe,
        core,
        #[cfg(windows)]
        session: None,
    }
}

fn uexplorer_data_dir() -> Result<PathBuf, String> {
    let base = std::env::var_os("LOCALAPPDATA")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .ok_or_else(|| {
            "LOCALAPPDATA_UNAVAILABLE: the Host configuration directory is unavailable".to_string()
        })?;
    if !base.is_absolute() {
        return Err(
            "LOCALAPPDATA_INVALID: the Host configuration directory must be absolute".to_string(),
        );
    }
    let dir = base.join("UExplorer");
    fs::create_dir_all(&dir).map_err(|e| format!("Create data dir failed: {e}"))?;
    Ok(dir)
}

fn connection_ini_path() -> Result<PathBuf, String> {
    Ok(uexplorer_data_dir()?.join("connection.ini"))
}

#[command]
fn save_connection_settings(port: u16, token: String) -> Result<bool, String> {
    let path = connection_ini_path()?;
    let port_mode = if port == 0 { "auto" } else { "fixed" };
    let content = format!(
        "[Connection]\nPreferredPort={}\nToken={}\nPortMode={}\n",
        port, token, port_mode
    );
    fs::write(&path, content).map_err(|e| format!("Write connection settings failed: {e}"))?;
    Ok(true)
}

// Scan all running processes via Windows API and filter likely Unreal/game processes.
#[command]
fn scan_ue_processes() -> Result<Vec<ProcessInfo>, String> {
    #[cfg(windows)]
    {
        scan_processes_internal()
    }
    #[cfg(not(windows))]
    {
        Err("Process scanning is only supported on Windows".to_string())
    }
}

#[cfg(windows)]
fn scan_processes_internal() -> Result<Vec<ProcessInfo>, String> {
    let mut processes = Vec::new();
    unsafe {
        let snapshot = OwnedHandle::new(
            CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
                .map_err(|e| format!("PROCESS_SNAPSHOT_FAILED: {}", windows_error_detail(&e)))?,
        );

        let mut entry = PROCESSENTRY32W {
            dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
            ..Default::default()
        };

        Process32FirstW(snapshot.raw(), &mut entry)
            .map_err(|e| format!("PROCESS_ENUMERATION_FAILED: {}", windows_error_detail(&e)))?;

        loop {
            let pid = entry.th32ProcessID;
            if pid > 0 {
                let name = utf16_z_to_string(&entry.szExeFile);
                if let Ok(details) = query_process_details(pid) {
                    let candidate_reasons = unreal_candidate_reasons(&name, &details.path);
                    if !candidate_reasons.is_empty() {
                        processes.push(ProcessInfo {
                            pid,
                            name,
                            path: details.path,
                            start_time_100ns: details.start_time_100ns.to_string(),
                            architecture: machine_name(details.machine).to_string(),
                            candidate_reasons,
                        });
                    }
                }
            }

            match Process32NextW(snapshot.raw(), &mut entry) {
                Ok(()) => {}
                Err(error) if is_no_more_files(&error) => {
                    break;
                }
                Err(error) => {
                    return Err(format!(
                        "PROCESS_ENUMERATION_FAILED: {}",
                        windows_error_detail(&error)
                    ));
                }
            }
        }
    }

    processes.sort_by(|a, b| {
        a.name
            .to_lowercase()
            .cmp(&b.name.to_lowercase())
            .then_with(|| a.pid.cmp(&b.pid))
    });

    Ok(processes)
}

#[cfg(windows)]
#[derive(Debug)]
struct OwnedHandle(usize);

#[cfg(windows)]
impl OwnedHandle {
    fn new(handle: HANDLE) -> Self {
        Self(handle.0 as usize)
    }

    fn raw(&self) -> HANDLE {
        HANDLE(self.0 as *mut c_void)
    }

    fn value(&self) -> usize {
        self.0
    }
}

#[cfg(windows)]
impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.raw());
        }
    }
}

#[cfg(windows)]
struct ProcessDetails {
    path: String,
    start_time_100ns: u64,
    machine: IMAGE_FILE_MACHINE,
}

#[cfg(windows)]
fn utf16_z_to_string(buf: &[u16]) -> String {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..end])
}

#[cfg(windows)]
fn query_process_details(pid: u32) -> Result<ProcessDetails, String> {
    unsafe {
        let handle = OwnedHandle::new(
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
                .map_err(|e| format!("PROCESS_OPEN_FAILED: {}", windows_error_detail(&e)))?,
        );
        Ok(ProcessDetails {
            path: query_process_path_from_handle(handle.raw())?,
            start_time_100ns: query_process_start_time(handle.raw())?,
            machine: query_process_machine(handle.raw())?,
        })
    }
}

#[cfg(windows)]
fn query_process_path_from_handle(handle: HANDLE) -> Result<String, String> {
    let mut path_buf = vec![0u16; 32768];
    let mut size = path_buf.len() as u32;
    unsafe {
        QueryFullProcessImageNameW(
            handle,
            PROCESS_NAME_WIN32,
            PWSTR(path_buf.as_mut_ptr()),
            &mut size,
        )
        .map_err(|e| format!("PROCESS_PATH_FAILED: {}", windows_error_detail(&e)))?;
    }
    if size == 0 {
        return Err("PROCESS_PATH_FAILED: Windows returned an empty path".to_string());
    }
    Ok(String::from_utf16_lossy(&path_buf[..size as usize]))
}

#[cfg(windows)]
fn query_process_start_time(handle: HANDLE) -> Result<u64, String> {
    let mut creation = FILETIME::default();
    let mut exit = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    unsafe {
        GetProcessTimes(handle, &mut creation, &mut exit, &mut kernel, &mut user)
            .map_err(|e| format!("PROCESS_TIME_FAILED: {}", windows_error_detail(&e)))?;
    }
    Ok(((creation.dwHighDateTime as u64) << 32) | creation.dwLowDateTime as u64)
}

#[cfg(windows)]
fn query_process_machine(handle: HANDLE) -> Result<IMAGE_FILE_MACHINE, String> {
    let mut process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    let mut native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    unsafe {
        IsWow64Process2(handle, &mut process_machine, Some(&mut native_machine))
            .map_err(|e| format!("PROCESS_ARCH_FAILED: {}", windows_error_detail(&e)))?;
    }
    Ok(if process_machine == IMAGE_FILE_MACHINE_UNKNOWN {
        native_machine
    } else {
        process_machine
    })
}

#[cfg(windows)]
fn machine_name(machine: IMAGE_FILE_MACHINE) -> &'static str {
    if machine == IMAGE_FILE_MACHINE_AMD64 {
        "x64"
    } else if machine == IMAGE_FILE_MACHINE_I386 {
        "x86"
    } else if machine == IMAGE_FILE_MACHINE_ARM64 {
        "arm64"
    } else {
        "unknown"
    }
}

#[cfg(windows)]
fn unreal_candidate_reasons(name: &str, path: &str) -> Vec<String> {
    let lower = name.to_lowercase();
    let path_lower = path.to_lowercase();
    let name_excludes = [
        "steam",
        "epic",
        "launcher",
        "updater",
        "helper",
        "service",
        "renderer",
        "crashreporter",
        "ue4prereq",
        "vc redist",
        "vcredist",
        "directx",
        ".net",
        "uexplorer",
        "app.exe",
        "explorer.exe",
        "msedge",
        "chrome",
        "firefox",
        "discord",
    ];
    let path_excludes = [
        "\\windows\\system32\\",
        "\\windows\\syswow64\\",
        "\\microsoft\\edge\\",
        "\\google\\chrome\\",
        "\\mozilla firefox\\",
    ];

    for ex in name_excludes {
        if lower.contains(ex) {
            return Vec::new();
        }
    }
    for ex in path_excludes {
        if path_lower.contains(ex) {
            return Vec::new();
        }
    }

    let explicit_markers = [
        "ue4editor",
        "ue5editor",
        "unrealeditor",
        "ue4",
        "ue5",
        "unreal",
    ];
    let mut direct_reasons = Vec::new();
    for marker in explicit_markers {
        if lower.contains(marker) {
            direct_reasons.push(format!("name:{marker}"));
        }
    }

    for marker in ["-win64-shipping", "-win64-development", "-win64-test"] {
        if lower.contains(marker) {
            direct_reasons.push(format!("name:{marker}"));
        }
    }
    if !direct_reasons.is_empty() {
        return direct_reasons;
    }

    let mut scored_reasons = Vec::new();
    let game_indicators = [
        "\\engine\\binaries\\",
        "\\binaries\\win64\\",
        "\\windowsnoeditor\\",
        "\\saved\\stagedbuilds\\",
        "\\unrealengine\\",
    ];

    for indicator in game_indicators {
        if path_lower.contains(indicator) {
            scored_reasons.push(format!("path:{indicator}"));
        }
    }

    if lower.contains("-win64-") {
        scored_reasons.push("name:-win64-".to_string());
    }
    if lower.ends_with(".exe") {
        scored_reasons.push("name:.exe".to_string());
    }

    if scored_reasons.len() >= 2 {
        scored_reasons
    } else {
        Vec::new()
    }
}

#[cfg(windows)]
fn windows_error_detail(error: &WindowsError) -> String {
    let hresult = error.code().0 as u32;
    let win32 = if hresult & 0xFFFF_0000 == 0x8007_0000 {
        Some(hresult & 0xFFFF)
    } else {
        None
    };
    match win32 {
        Some(code) => format!("Win32={code}, HRESULT=0x{hresult:08X}: {error}"),
        None => format!("HRESULT=0x{hresult:08X}: {error}"),
    }
}

#[cfg(windows)]
fn last_windows_error_detail() -> String {
    let code = unsafe { GetLastError() };
    let error = WindowsError::from_hresult(HRESULT::from_win32(code.0));
    windows_error_detail(&error)
}

#[cfg(windows)]
fn is_no_more_files(error: &WindowsError) -> bool {
    error.code() == HRESULT::from_win32(ERROR_NO_MORE_FILES.0)
}

#[cfg(windows)]
const CORE_CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
#[cfg(windows)]
const SESSION_CLOSE_TIMEOUT: Duration = Duration::from_secs(5);
#[cfg(windows)]
const MAX_CONCURRENT_TARGET_OPERATIONS: usize = 16;

#[cfg(windows)]
#[derive(Debug, Default)]
struct TargetOperationCoordinator {
    target_pids: Mutex<BTreeSet<u32>>,
}

#[cfg(windows)]
#[derive(Debug)]
struct TargetOperationReservation {
    coordinator: Arc<TargetOperationCoordinator>,
    target_pid: u32,
}

#[cfg(windows)]
impl TargetOperationCoordinator {
    fn reserve(
        self: &Arc<Self>,
        target_pid: u32,
    ) -> Result<TargetOperationReservation, InjectionError> {
        if target_pid == 0 {
            return Err(InjectionError::failed(
                "TARGET_OPERATION_PID_INVALID",
                "admission",
                "target PID must be positive",
            ));
        }
        let mut target_pids = self.target_pids.lock().map_err(|_| {
            InjectionError::failed(
                "TARGET_OPERATION_LOCK_POISONED",
                "admission",
                "target operation coordinator lock is poisoned",
            )
        })?;
        if target_pids.contains(&target_pid) {
            return Err(InjectionError::failed(
                "TARGET_OPERATION_IN_PROGRESS",
                "admission",
                format!("PID {target_pid} already has a managed operation in progress"),
            ));
        }
        if target_pids.len() >= MAX_CONCURRENT_TARGET_OPERATIONS {
            return Err(InjectionError::failed(
                "TARGET_OPERATION_LIMIT_REACHED",
                "admission",
                format!("at most {MAX_CONCURRENT_TARGET_OPERATIONS} target operations may run"),
            ));
        }
        target_pids.insert(target_pid);
        Ok(TargetOperationReservation {
            coordinator: Arc::clone(self),
            target_pid,
        })
    }
}

#[cfg(windows)]
impl Drop for TargetOperationReservation {
    fn drop(&mut self) {
        match self.coordinator.target_pids.lock() {
            Ok(mut target_pids) => {
                target_pids.remove(&self.target_pid);
            }
            Err(poisoned) => {
                poisoned.into_inner().remove(&self.target_pid);
            }
        }
    }
}

// Injection is complete only after the PID-scoped Pipe returns a validated Ready Welcome.
#[cfg(windows)]
#[command]
async fn inject_and_connect(
    manager: State<'_, Arc<SessionManager>>,
    coordinator: State<'_, Arc<TargetOperationCoordinator>>,
    pid: u32,
    dll_path: String,
    expected_start_time_100ns: String,
    expected_process_path: String,
) -> Result<InjectionResult, String> {
    let manager = Arc::clone(manager.inner());
    let reservation = match coordinator.inner().reserve(pid) {
        Ok(reservation) => reservation,
        Err(error) => return Ok(error.into_failure(DllLoadState::NotAttempted)),
    };
    match tauri::async_runtime::spawn_blocking(move || {
        let _reservation = reservation;
        inject_and_connect_internal(
            &manager,
            pid,
            &dll_path,
            &expected_start_time_100ns,
            &expected_process_path,
        )
    })
    .await
    {
        Ok(result) => Ok(result),
        Err(error) => Ok(injection_failure(
            "host_task",
            "HOST_TASK_FAILED",
            error.to_string(),
            DllLoadState::Failed,
            PipeConnectionState::NotAttempted,
            CoreReadinessState::NotChecked,
        )),
    }
}

#[cfg(not(windows))]
#[command]
async fn inject_and_connect(
    pid: u32,
    dll_path: String,
    expected_start_time_100ns: String,
    expected_process_path: String,
) -> Result<InjectionResult, String> {
    let _ = (
        pid,
        dll_path,
        expected_start_time_100ns,
        expected_process_path,
    );
    Ok(injection_failure(
        "preflight",
        "PLATFORM_UNSUPPORTED",
        "DLL injection and Core sessions are only supported on Windows",
        DllLoadState::NotAttempted,
        PipeConnectionState::NotAttempted,
        CoreReadinessState::NotChecked,
    ))
}

#[cfg(windows)]
#[command]
fn session_diagnostics(
    manager: State<'_, Arc<SessionManager>>,
) -> Result<SessionManagerDiagnostics, String> {
    manager
        .diagnostics()
        .map_err(|error| format!("{}: {error}", error.code()))
}

#[cfg(not(windows))]
#[command]
fn session_diagnostics() -> Result<serde_json::Value, String> {
    Err("PLATFORM_UNSUPPORTED: Core sessions are only supported on Windows".to_string())
}

#[cfg(windows)]
#[command]
fn activate_session(manager: State<'_, Arc<SessionManager>>, pid: u32) -> Result<bool, String> {
    manager
        .activate(pid)
        .map(|()| true)
        .map_err(|error| format!("{}: {error}", error.code()))
}

#[cfg(not(windows))]
#[command]
fn activate_session(pid: u32) -> Result<bool, String> {
    let _ = pid;
    Err("PLATFORM_UNSUPPORTED: Core sessions are only supported on Windows".to_string())
}

struct TauriChannelEventSink(Channel<HostEvent>);

impl EventSink for TauriChannelEventSink {
    fn send(&self, event: HostEvent) -> Result<(), String> {
        self.0
            .send(event)
            .map_err(|error| format!("TAURI_CHANNEL_SEND_FAILED: {error}"))
    }
}

#[cfg(windows)]
#[command]
fn subscribe_session_events(
    manager: State<'_, Arc<SessionManager>>,
    bridges: State<'_, Arc<EventBridgeManager>>,
    pid: u32,
    filter: EventFilter,
    replay_after_seq: Option<u64>,
    capacity: usize,
    on_event: Channel<HostEvent>,
) -> Result<EventBridgeDiagnostics, String> {
    let session = manager
        .get(pid)
        .map_err(|error| format!("{}: {error}", error.code()))?;
    let subscription = session
        .subscribe_events(filter, replay_after_seq, capacity)
        .map_err(|error| format!("{}: {error}", error.code()))?;
    bridges
        .subscribe(pid, subscription, Box::new(TauriChannelEventSink(on_event)))
        .map_err(|error| format!("{}: {error}", error.code()))
}

#[cfg(not(windows))]
#[command]
fn subscribe_session_events(
    pid: u32,
    filter: EventFilter,
    replay_after_seq: Option<u64>,
    capacity: usize,
    on_event: Channel<HostEvent>,
) -> Result<EventBridgeDiagnostics, String> {
    let _ = (pid, filter, replay_after_seq, capacity, on_event);
    Err("PLATFORM_UNSUPPORTED: Core sessions are only supported on Windows".to_string())
}

#[cfg(windows)]
#[command]
async fn unsubscribe_session_events(
    bridges: State<'_, Arc<EventBridgeManager>>,
    bridge_id: u64,
) -> Result<bool, String> {
    let bridges = Arc::clone(bridges.inner());
    tauri::async_runtime::spawn_blocking(move || bridges.unsubscribe(bridge_id))
        .await
        .map_err(|error| format!("HOST_TASK_FAILED: {error}"))?
        .map(|()| true)
        .map_err(|error| format!("{}: {error}", error.code()))
}

#[cfg(not(windows))]
#[command]
async fn unsubscribe_session_events(bridge_id: u64) -> Result<bool, String> {
    let _ = bridge_id;
    Err("PLATFORM_UNSUPPORTED: Core sessions are only supported on Windows".to_string())
}

#[cfg(windows)]
#[command]
fn event_bridge_diagnostics(
    bridges: State<'_, Arc<EventBridgeManager>>,
) -> Result<Vec<EventBridgeDiagnostics>, String> {
    bridges
        .diagnostics()
        .map_err(|error| format!("{}: {error}", error.code()))
}

#[cfg(not(windows))]
#[command]
fn event_bridge_diagnostics() -> Result<Vec<EventBridgeDiagnostics>, String> {
    Err("PLATFORM_UNSUPPORTED: Core sessions are only supported on Windows".to_string())
}

#[cfg(windows)]
#[command]
async fn disconnect_session(
    manager: State<'_, Arc<SessionManager>>,
    coordinator: State<'_, Arc<TargetOperationCoordinator>>,
    bridges: State<'_, Arc<EventBridgeManager>>,
    pid: u32,
    reason: String,
) -> Result<bool, String> {
    let manager = Arc::clone(manager.inner());
    let bridges = Arc::clone(bridges.inner());
    let reservation = coordinator
        .inner()
        .reserve(pid)
        .map_err(|error| format!("{}: {}", error.code, error.detail))?;
    tauri::async_runtime::spawn_blocking(move || {
        let _reservation = reservation;
        let bridge_result = bridges
            .stop_session(pid)
            .map_err(|error| format!("{}: {error}", error.code()));
        let session_result = manager
            .disconnect(pid, &reason, SESSION_CLOSE_TIMEOUT)
            .map_err(|error| format!("{}: {error}", error.code()));
        match (bridge_result, session_result) {
            (Ok(_), Ok(())) => Ok(()),
            (Err(error), Ok(())) | (Ok(_), Err(error)) => Err(error),
            (Err(bridge_error), Err(session_error)) => Err(format!(
                "SESSION_DISCONNECT_MULTIPLE_FAILURES: {bridge_error}; {session_error}"
            )),
        }
    })
    .await
    .map_err(|error| format!("HOST_TASK_FAILED: {error}"))?
    .map(|()| true)
}

#[cfg(not(windows))]
#[command]
async fn disconnect_session(pid: u32, reason: String) -> Result<bool, String> {
    let _ = (pid, reason);
    Err("PLATFORM_UNSUPPORTED: Core sessions are only supported on Windows".to_string())
}

#[cfg(windows)]
const INJECTION_TIMEOUT_MS: u32 = 10_000;
#[cfg(windows)]
const PE_MACHINE_AMD64: u16 = 0x8664;
#[cfg(windows)]
const PE32_PLUS_MAGIC: u16 = 0x020B;

#[cfg(windows)]
#[derive(Clone, Copy, Debug)]
pub struct InjectionTimeouts {
    pub remote_thread_ms: u32,
    pub core_connect: Duration,
}

#[cfg(windows)]
impl InjectionTimeouts {
    pub const fn production() -> Self {
        Self {
            remote_thread_ms: INJECTION_TIMEOUT_MS,
            core_connect: CORE_CONNECT_TIMEOUT,
        }
    }
}

#[cfg(windows)]
#[derive(Debug)]
struct InjectionError {
    code: &'static str,
    stage: &'static str,
    status: &'static str,
    detail: String,
}

#[cfg(windows)]
impl InjectionError {
    fn failed(code: &'static str, stage: &'static str, detail: impl Into<String>) -> Self {
        Self {
            code,
            stage,
            status: "failed",
            detail: detail.into(),
        }
    }

    fn already_loaded(detail: impl Into<String>) -> Self {
        Self {
            code: "DLL_ALREADY_LOADED",
            stage: "preflight",
            status: "already_loaded",
            detail: detail.into(),
        }
    }

    fn is_already_loaded(&self) -> bool {
        self.status == "already_loaded"
    }

    fn dll_state_on_failure(&self) -> DllLoadState {
        if self.stage == "preflight" || self.stage == "admission" {
            return DllLoadState::NotAttempted;
        }
        match (self.stage, self.code) {
            (
                "remote_wait",
                "REMOTE_THREAD_TIMEOUT"
                | "REMOTE_THREAD_WAIT_FAILED"
                | "REMOTE_THREAD_WAIT_UNEXPECTED",
            )
            | (
                "verify_load",
                "REMOTE_EXIT_CODE_FAILED" | "MODULE_SNAPSHOT_FAILED" | "MODULE_ENUMERATION_FAILED",
            )
            | ("cleanup", "CLEANUP_THREAD_CREATE_FAILED") => DllLoadState::Indeterminate,
            ("cleanup", "REMOTE_FREE_FAILED") => DllLoadState::Loaded,
            _ => DllLoadState::Failed,
        }
    }

    fn into_failure(self, dll: DllLoadState) -> InjectionResult {
        injection_failure(
            self.stage,
            self.code,
            self.detail,
            dll,
            PipeConnectionState::NotAttempted,
            CoreReadinessState::NotChecked,
        )
    }
}

#[cfg(windows)]
struct DllLoadOutcome {
    message: String,
}

#[cfg(windows)]
fn inject_and_connect_internal(
    manager: &SessionManager,
    pid: u32,
    dll_path: &str,
    expected_start_time_100ns: &str,
    expected_process_path: &str,
) -> InjectionResult {
    inject_and_connect_target(
        manager,
        pid,
        dll_path,
        expected_start_time_100ns,
        expected_process_path,
        InjectionTimeouts::production(),
    )
}

#[cfg(windows)]
pub fn inject_and_connect_target(
    manager: &SessionManager,
    pid: u32,
    dll_path: &str,
    expected_start_time_100ns: &str,
    expected_process_path: &str,
    timeouts: InjectionTimeouts,
) -> InjectionResult {
    if timeouts.remote_thread_ms == 0 || timeouts.core_connect.is_zero() {
        return injection_failure(
            "preflight",
            "INJECTION_TIMEOUT_INVALID",
            "remote thread and Core connect timeouts must be positive",
            DllLoadState::NotAttempted,
            PipeConnectionState::NotAttempted,
            CoreReadinessState::NotChecked,
        );
    }
    let (dll_state, dll_message) = match perform_injection(
        pid,
        dll_path,
        expected_start_time_100ns,
        expected_process_path,
        timeouts.remote_thread_ms,
    ) {
        Ok(outcome) => (DllLoadState::Loaded, outcome.message),
        Err(error) if error.is_already_loaded() => (DllLoadState::AlreadyLoaded, error.detail),
        Err(error) => {
            let dll = error.dll_state_on_failure();
            return error.into_failure(dll);
        }
    };

    let target = match validated_target_identity(
        pid,
        expected_start_time_100ns,
        expected_process_path,
        "post_load_identity",
    ) {
        Ok(identity) => identity,
        Err(error) => return error.into_failure(dll_state),
    };

    match manager.get(pid) {
        Ok(session) => {
            if session.target_identity() != &target {
                return injection_failure(
                    "session_admission",
                    "SESSION_PROCESS_IDENTITY_MISMATCH",
                    "an existing session for this PID belongs to different process evidence",
                    dll_state,
                    PipeConnectionState::Rejected,
                    CoreReadinessState::NotChecked,
                );
            }
            let diagnostics = match session.diagnostics() {
                Ok(diagnostics) => diagnostics,
                Err(error) => {
                    return session_failure_result(error, dll_state);
                }
            };
            if diagnostics.phase != SessionPhase::Ready {
                return injection_failure(
                    "session_admission",
                    "SESSION_NOT_READY",
                    format!("existing PID {pid} session is {:?}", diagnostics.phase),
                    dll_state,
                    PipeConnectionState::Failed,
                    CoreReadinessState::Failed,
                );
            }
            if let Err(error) = manager.activate(pid) {
                return session_failure_result(error, dll_state);
            }
            return injection_ready(
                "CORE_ALREADY_READY",
                format!("{dll_message}; existing Core session is ready"),
                dll_state,
                diagnostics,
            );
        }
        Err(SessionManagerError::SessionNotFound(_)) => {}
        Err(error) => return session_failure_result(error, dll_state),
    }

    let session = match manager.connect(
        target.clone(),
        concat!("uexplorer-host/", env!("CARGO_PKG_VERSION")),
        timeouts.core_connect,
    ) {
        Ok(session) => session,
        Err(error) => return session_failure_result(error, dll_state),
    };

    if let Err(error) = validated_target_identity(
        pid,
        expected_start_time_100ns,
        expected_process_path,
        "post_connect_identity",
    ) {
        let _ = manager.abort_connection(pid, SESSION_CLOSE_TIMEOUT);
        return injection_failure(
            error.stage,
            error.code,
            error.detail,
            dll_state,
            PipeConnectionState::Connected,
            CoreReadinessState::Ready,
        );
    }
    if let Err(error) = manager.activate(pid) {
        let _ = manager.disconnect(pid, "activation_failed", SESSION_CLOSE_TIMEOUT);
        return session_failure_result(error, dll_state);
    }
    let diagnostics = match session.diagnostics() {
        Ok(diagnostics) => diagnostics,
        Err(error) => {
            let _ = manager.disconnect(pid, "diagnostics_failed", SESSION_CLOSE_TIMEOUT);
            return session_failure_result(error, dll_state);
        }
    };
    injection_ready(
        "CORE_READY",
        format!("{dll_message}; PID-scoped Pipe connected and Core Ready validated"),
        dll_state,
        diagnostics,
    )
}

#[cfg(windows)]
fn injection_ready(
    code: &str,
    message: String,
    dll: DllLoadState,
    session: ManagedSessionDiagnostics,
) -> InjectionResult {
    InjectionResult {
        success: true,
        status: InjectionStatus::Ready,
        stage: "core_ready".to_string(),
        code: code.to_string(),
        message,
        dll,
        pipe: PipeConnectionState::Connected,
        core: CoreReadinessState::Ready,
        session: Some(session),
    }
}

#[cfg(windows)]
fn session_failure_result(error: SessionManagerError, dll: DllLoadState) -> InjectionResult {
    let (stage, pipe, core) = classify_session_failure(&error);
    injection_failure(stage, error.code(), error.to_string(), dll, pipe, core)
}

#[cfg(windows)]
fn classify_session_failure(
    error: &SessionManagerError,
) -> (&'static str, PipeConnectionState, CoreReadinessState) {
    match error {
        SessionManagerError::Client(CoreRpcClientError::ConnectTimeout { .. }) => (
            "pipe_connect",
            PipeConnectionState::Failed,
            CoreReadinessState::NotChecked,
        ),
        SessionManagerError::Client(CoreRpcClientError::PeerPidMismatch { .. }) => (
            "pipe_identity",
            PipeConnectionState::Rejected,
            CoreReadinessState::NotChecked,
        ),
        SessionManagerError::Client(CoreRpcClientError::Transport { code, .. })
            if matches!(
                code.as_str(),
                "RPC_PIPE_OPEN_FAILED" | "RPC_CONNECT_WAIT_FAILED" | "RPC_WORKER_CREATE_FAILED"
            ) =>
        {
            (
                "pipe_connect",
                PipeConnectionState::Failed,
                CoreReadinessState::NotChecked,
            )
        }
        SessionManagerError::Client(CoreRpcClientError::Transport { code, .. })
            if matches!(
                code.as_str(),
                "RPC_SERVER_PID_INVALID" | "RPC_SERVER_PID_QUERY_FAILED" | "RPC_PIPE_MODE_FAILED"
            ) =>
        {
            (
                "pipe_identity",
                PipeConnectionState::Rejected,
                CoreReadinessState::NotChecked,
            )
        }
        SessionManagerError::Client(CoreRpcClientError::Session { .. })
        | SessionManagerError::Client(CoreRpcClientError::Transport { .. })
        | SessionManagerError::Client(CoreRpcClientError::WorkerStopped)
        | SessionManagerError::Client(CoreRpcClientError::WorkerPanicked) => (
            "core_ready",
            PipeConnectionState::Connected,
            CoreReadinessState::Failed,
        ),
        SessionManagerError::WorkerCreate(_)
        | SessionManagerError::EventHub(_)
        | SessionManagerError::Snapshot(_) => (
            "session_start",
            PipeConnectionState::Connected,
            CoreReadinessState::Ready,
        ),
        _ => (
            "session_admission",
            PipeConnectionState::NotAttempted,
            CoreReadinessState::NotChecked,
        ),
    }
}

#[cfg(windows)]
fn validated_target_identity(
    pid: u32,
    expected_start_time_100ns: &str,
    expected_process_path: &str,
    stage: &'static str,
) -> Result<TargetProcessIdentity, InjectionError> {
    let expected_start_time = expected_start_time_100ns.parse::<u64>().map_err(|error| {
        InjectionError::failed(
            "PROCESS_IDENTITY_INVALID",
            stage,
            format!("invalid start_time_100ns '{expected_start_time_100ns}': {error}"),
        )
    })?;
    if expected_start_time == 0 || expected_process_path.trim().is_empty() {
        return Err(InjectionError::failed(
            "PROCESS_IDENTITY_INVALID",
            stage,
            "a non-zero process start time and process path are required",
        ));
    }
    let details = query_process_details(pid)
        .map_err(|detail| InjectionError::failed("PROCESS_IDENTITY_QUERY_FAILED", stage, detail))?;
    if details.start_time_100ns != expected_start_time
        || normalize_path_key(&details.path) != normalize_path_key(expected_process_path)
    {
        return Err(InjectionError::failed(
            "PROCESS_IDENTITY_MISMATCH",
            stage,
            format!("PID {pid} no longer matches the selected start time and process path"),
        ));
    }
    if details.machine != IMAGE_FILE_MACHINE_AMD64 {
        return Err(InjectionError::failed(
            "TARGET_ARCH_MISMATCH",
            stage,
            format!(
                "PID {pid} is {}; x64 is required",
                machine_name(details.machine)
            ),
        ));
    }
    TargetProcessIdentity::new(pid, details.start_time_100ns, details.path).map_err(|error| {
        InjectionError::failed("PROCESS_IDENTITY_INVALID", stage, error.to_string())
    })
}

#[cfg(windows)]
struct DllIdentity {
    canonical_path: PathBuf,
    path_key: String,
    wide_path: Vec<u16>,
    file_size: u64,
}

#[cfg(windows)]
struct ModuleSnapshotEntry {
    path_key: String,
    module_key: String,
    base: usize,
    size: usize,
}

#[cfg(windows)]
struct RemoteAllocation {
    process: usize,
    address: usize,
}

#[cfg(windows)]
impl RemoteAllocation {
    fn new(process: HANDLE, address: *mut c_void) -> Self {
        Self {
            process: process.0 as usize,
            address: address as usize,
        }
    }

    fn ptr(&self) -> *mut c_void {
        self.address as *mut c_void
    }

    fn release(&mut self) -> Result<(), InjectionError> {
        if self.address == 0 {
            return Ok(());
        }
        let result = unsafe {
            VirtualFreeEx(
                HANDLE(self.process as *mut c_void),
                self.ptr(),
                0,
                MEM_RELEASE,
            )
        };
        match result {
            Ok(()) => {
                self.address = 0;
                Ok(())
            }
            Err(error) => Err(InjectionError::failed(
                "REMOTE_FREE_FAILED",
                "cleanup",
                windows_error_detail(&error),
            )),
        }
    }
}

#[cfg(windows)]
impl Drop for RemoteAllocation {
    fn drop(&mut self) {
        if self.address == 0 {
            return;
        }
        let result = unsafe {
            VirtualFreeEx(
                HANDLE(self.process as *mut c_void),
                self.ptr(),
                0,
                MEM_RELEASE,
            )
        };
        if let Err(error) = result {
            log::error!(
                "REMOTE_FREE_FAILED during drop: {}",
                windows_error_detail(&error)
            );
        }
    }
}

#[cfg(windows)]
fn perform_injection(
    pid: u32,
    dll_path: &str,
    expected_start_time_100ns: &str,
    expected_process_path: &str,
    remote_thread_timeout_ms: u32,
) -> Result<DllLoadOutcome, InjectionError> {
    if !cfg!(target_pointer_width = "64") {
        return Err(InjectionError::failed(
            "HOST_ARCH_MISMATCH",
            "preflight",
            "UExplorer Host must be built as x64",
        ));
    }

    let expected_start_time = expected_start_time_100ns.parse::<u64>().map_err(|error| {
        InjectionError::failed(
            "PROCESS_IDENTITY_INVALID",
            "preflight",
            format!("invalid start_time_100ns '{expected_start_time_100ns}': {error}"),
        )
    })?;
    if expected_start_time == 0 || expected_process_path.trim().is_empty() {
        return Err(InjectionError::failed(
            "PROCESS_IDENTITY_INVALID",
            "preflight",
            "a non-zero process start time and process path are required",
        ));
    }

    let dll = prepare_dll_identity(dll_path)?;
    let desired_access = PROCESS_CREATE_THREAD
        | PROCESS_QUERY_INFORMATION
        | PROCESS_VM_OPERATION
        | PROCESS_VM_WRITE
        | PROCESS_VM_READ;
    let process = OwnedHandle::new(unsafe { OpenProcess(desired_access, false, pid) }.map_err(
        |error| {
            InjectionError::failed(
                "PROCESS_OPEN_FAILED",
                "preflight",
                format!("PID {pid}: {}", windows_error_detail(&error)),
            )
        },
    )?);

    let actual_start_time = query_process_start_time(process.raw()).map_err(|detail| {
        InjectionError::failed("PROCESS_IDENTITY_QUERY_FAILED", "preflight", detail)
    })?;
    if actual_start_time != expected_start_time {
        return Err(InjectionError::failed(
            "PROCESS_IDENTITY_MISMATCH",
            "preflight",
            format!(
                "PID {pid} was reused or restarted: expected {expected_start_time}, actual {actual_start_time}"
            ),
        ));
    }

    let actual_process_path = query_process_path_from_handle(process.raw()).map_err(|detail| {
        InjectionError::failed("PROCESS_IDENTITY_QUERY_FAILED", "preflight", detail)
    })?;
    if normalize_path_key(&actual_process_path) != normalize_path_key(expected_process_path) {
        return Err(InjectionError::failed(
            "PROCESS_IDENTITY_MISMATCH",
            "preflight",
            format!(
                "PID {pid} path changed: expected '{expected_process_path}', actual '{actual_process_path}'"
            ),
        ));
    }

    let target_machine = query_process_machine(process.raw()).map_err(|detail| {
        InjectionError::failed("TARGET_ARCH_QUERY_FAILED", "preflight", detail)
    })?;
    if target_machine != IMAGE_FILE_MACHINE_AMD64 {
        return Err(InjectionError::failed(
            "TARGET_ARCH_MISMATCH",
            "preflight",
            format!(
                "PID {pid} is {} (machine 0x{:04X}); x64 is required",
                machine_name(target_machine),
                target_machine.0
            ),
        ));
    }

    if module_path_is_loaded(pid, &dll.path_key, "preflight")? {
        return Err(InjectionError::already_loaded(format!(
            "{} is already loaded in PID {pid}",
            dll.canonical_path.display()
        )));
    }

    let memory_size = dll.wide_path.len() * std::mem::size_of::<u16>();
    let remote_address = unsafe {
        VirtualAllocEx(
            process.raw(),
            None,
            memory_size,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE,
        )
    };
    if remote_address.is_null() {
        return Err(InjectionError::failed(
            "REMOTE_ALLOC_FAILED",
            "remote_write",
            last_windows_error_detail(),
        ));
    }
    let mut remote = RemoteAllocation::new(process.raw(), remote_address);

    let mut bytes_written = 0usize;
    unsafe {
        WriteProcessMemory(
            process.raw(),
            remote.ptr(),
            dll.wide_path.as_ptr().cast(),
            memory_size,
            Some(&mut bytes_written),
        )
    }
    .map_err(|error| {
        InjectionError::failed(
            "REMOTE_WRITE_FAILED",
            "remote_write",
            windows_error_detail(&error),
        )
    })?;
    if bytes_written != memory_size {
        return Err(InjectionError::failed(
            "REMOTE_WRITE_INCOMPLETE",
            "remote_write",
            format!("expected {memory_size} bytes, wrote {bytes_written}"),
        ));
    }

    let kernel32_name: Vec<u16> = "kernel32.dll\0".encode_utf16().collect();
    let kernel32 =
        unsafe { GetModuleHandleW(PCWSTR(kernel32_name.as_ptr())) }.map_err(|error| {
            InjectionError::failed(
                "KERNEL32_LOOKUP_FAILED",
                "remote_thread",
                windows_error_detail(&error),
            )
        })?;
    let load_library =
        unsafe { GetProcAddress(kernel32, PCSTR(c"LoadLibraryW".as_ptr() as *const u8)) }
            .ok_or_else(|| {
                InjectionError::failed(
                    "LOAD_LIBRARY_LOOKUP_FAILED",
                    "remote_thread",
                    "GetProcAddress returned NULL for LoadLibraryW",
                )
            })?;
    let start_routine = resolve_remote_load_library(pid, load_library)?;

    let remote_thread = OwnedHandle::new(
        unsafe {
            CreateRemoteThread(
                process.raw(),
                None,
                0,
                Some(start_routine),
                Some(remote.ptr()),
                0,
                None,
            )
        }
        .map_err(|error| {
            InjectionError::failed(
                "REMOTE_THREAD_CREATE_FAILED",
                "remote_thread",
                windows_error_detail(&error),
            )
        })?,
    );

    let wait_result = unsafe { WaitForSingleObject(remote_thread.raw(), remote_thread_timeout_ms) };
    if wait_result == WAIT_TIMEOUT {
        defer_injection_cleanup(process, remote_thread, remote)?;
        return Err(InjectionError::failed(
            "REMOTE_THREAD_TIMEOUT",
            "remote_wait",
            format!(
                "LoadLibraryW did not finish within {remote_thread_timeout_ms} ms; cleanup will occur only after the thread exits"
            ),
        ));
    }
    if wait_result == WAIT_FAILED {
        let detail = last_windows_error_detail();
        defer_injection_cleanup(process, remote_thread, remote)?;
        return Err(InjectionError::failed(
            "REMOTE_THREAD_WAIT_FAILED",
            "remote_wait",
            detail,
        ));
    }
    if wait_result != WAIT_OBJECT_0 {
        defer_injection_cleanup(process, remote_thread, remote)?;
        return Err(InjectionError::failed(
            "REMOTE_THREAD_WAIT_UNEXPECTED",
            "remote_wait",
            format!("WaitForSingleObject returned 0x{:08X}", wait_result.0),
        ));
    }

    let mut exit_code = 0u32;
    unsafe { GetExitCodeThread(remote_thread.raw(), &mut exit_code) }.map_err(|error| {
        InjectionError::failed(
            "REMOTE_EXIT_CODE_FAILED",
            "verify_load",
            windows_error_detail(&error),
        )
    })?;
    if exit_code == 0 {
        return Err(InjectionError::failed(
            "LOAD_LIBRARY_RETURNED_NULL",
            "verify_load",
            "LoadLibraryW returned NULL",
        ));
    }
    if !module_path_is_loaded(pid, &dll.path_key, "verify_load")? {
        return Err(InjectionError::failed(
            "DLL_MODULE_NOT_FOUND",
            "verify_load",
            format!(
                "remote thread returned 0x{exit_code:08X}, but '{}' is absent from the target module list",
                dll.canonical_path.display()
            ),
        ));
    }

    remote.release()?;
    Ok(DllLoadOutcome {
        message: format!("DLL load confirmed in PID {pid} ({} bytes)", dll.file_size),
    })
}

#[cfg(windows)]
fn prepare_dll_identity(dll_path: &str) -> Result<DllIdentity, InjectionError> {
    let requested = PathBuf::from(dll_path);
    let canonical_path = fs::canonicalize(&requested).map_err(|error| {
        InjectionError::failed(
            "DLL_CANONICALIZE_FAILED",
            "preflight",
            format!("'{}': {}", requested.display(), io_error_detail(&error)),
        )
    })?;
    let metadata = fs::metadata(&canonical_path).map_err(|error| {
        InjectionError::failed(
            "DLL_METADATA_FAILED",
            "preflight",
            format!(
                "'{}': {}",
                canonical_path.display(),
                io_error_detail(&error)
            ),
        )
    })?;
    if !metadata.is_file() {
        return Err(InjectionError::failed(
            "DLL_NOT_REGULAR_FILE",
            "preflight",
            canonical_path.display().to_string(),
        ));
    }
    let extension_is_dll = canonical_path
        .extension()
        .and_then(|value| value.to_str())
        .is_some_and(|value| value.eq_ignore_ascii_case("dll"));
    if !extension_is_dll {
        return Err(InjectionError::failed(
            "DLL_EXTENSION_INVALID",
            "preflight",
            canonical_path.display().to_string(),
        ));
    }

    let bytes = fs::read(&canonical_path).map_err(|error| {
        InjectionError::failed(
            "DLL_READ_FAILED",
            "preflight",
            format!(
                "'{}': {}",
                canonical_path.display(),
                io_error_detail(&error)
            ),
        )
    })?;
    validate_amd64_dll_bytes(&bytes).map_err(|reason| {
        InjectionError::failed(
            "DLL_PE_INVALID",
            "preflight",
            format!("'{}': {reason}", canonical_path.display()),
        )
    })?;

    let mut wide_path: Vec<u16> = canonical_path.as_os_str().encode_wide().collect();
    if wide_path.len() >= 260 {
        return Err(InjectionError::failed(
            "DLL_PATH_TOO_LONG",
            "preflight",
            "the verified Toolhelp module path must fit within 259 UTF-16 code units",
        ));
    }
    wide_path.push(0);
    Ok(DllIdentity {
        path_key: normalize_path_key(&canonical_path.to_string_lossy()),
        canonical_path,
        wide_path,
        file_size: metadata.len(),
    })
}

#[cfg(windows)]
fn validate_amd64_dll_bytes(bytes: &[u8]) -> Result<(), &'static str> {
    if bytes.len() < 0x40 || &bytes[..2] != b"MZ" {
        return Err("missing DOS header");
    }
    let pe_offset = u32::from_le_bytes(bytes[0x3C..0x40].try_into().unwrap()) as usize;
    let coff_end = pe_offset
        .checked_add(26)
        .ok_or("PE header offset overflow")?;
    if coff_end > bytes.len() || &bytes[pe_offset..pe_offset + 4] != b"PE\0\0" {
        return Err("missing or truncated PE signature");
    }
    let machine = u16::from_le_bytes(bytes[pe_offset + 4..pe_offset + 6].try_into().unwrap());
    if machine != PE_MACHINE_AMD64 {
        return Err("PE machine is not AMD64");
    }
    let optional_magic =
        u16::from_le_bytes(bytes[pe_offset + 24..pe_offset + 26].try_into().unwrap());
    if optional_magic != PE32_PLUS_MAGIC {
        return Err("optional header is not PE32+");
    }
    Ok(())
}

#[cfg(windows)]
fn module_path_is_loaded(
    pid: u32,
    expected_path_key: &str,
    stage: &'static str,
) -> Result<bool, InjectionError> {
    Ok(snapshot_modules(pid, stage)?
        .iter()
        .any(|module| module.path_key == expected_path_key))
}

#[cfg(windows)]
fn snapshot_modules(
    pid: u32,
    stage: &'static str,
) -> Result<Vec<ModuleSnapshotEntry>, InjectionError> {
    let snapshot = OwnedHandle::new(
        unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid) }.map_err(
            |error| {
                InjectionError::failed(
                    "MODULE_SNAPSHOT_FAILED",
                    stage,
                    windows_error_detail(&error),
                )
            },
        )?,
    );
    let mut entry = MODULEENTRY32W {
        dwSize: std::mem::size_of::<MODULEENTRY32W>() as u32,
        ..Default::default()
    };
    unsafe { Module32FirstW(snapshot.raw(), &mut entry) }.map_err(|error| {
        InjectionError::failed(
            "MODULE_ENUMERATION_FAILED",
            stage,
            windows_error_detail(&error),
        )
    })?;

    let mut modules = Vec::new();
    loop {
        modules.push(ModuleSnapshotEntry {
            path_key: normalize_path_key(&utf16_z_to_string(&entry.szExePath)),
            module_key: utf16_z_to_string(&entry.szModule).to_lowercase(),
            base: entry.modBaseAddr as usize,
            size: entry.modBaseSize as usize,
        });
        match unsafe { Module32NextW(snapshot.raw(), &mut entry) } {
            Ok(()) => {}
            Err(error) if is_no_more_files(&error) => return Ok(modules),
            Err(error) => {
                return Err(InjectionError::failed(
                    "MODULE_ENUMERATION_FAILED",
                    stage,
                    windows_error_detail(&error),
                ));
            }
        }
    }
}

#[cfg(windows)]
fn resolve_remote_load_library(
    target_pid: u32,
    local_load_library: unsafe extern "system" fn() -> isize,
) -> Result<unsafe extern "system" fn(*mut c_void) -> u32, InjectionError> {
    let local_address = local_load_library as usize;
    let local_modules = snapshot_modules(std::process::id(), "remote_thread")?;
    let owner = local_modules
        .iter()
        .find(|module| {
            module
                .base
                .checked_add(module.size)
                .is_some_and(|end| local_address >= module.base && local_address < end)
        })
        .ok_or_else(|| {
            InjectionError::failed(
                "LOAD_LIBRARY_OWNER_NOT_FOUND",
                "remote_thread",
                format!("local address 0x{local_address:X} is outside every module"),
            )
        })?;
    let rva = local_address.checked_sub(owner.base).ok_or_else(|| {
        InjectionError::failed(
            "LOAD_LIBRARY_RVA_INVALID",
            "remote_thread",
            "LoadLibraryW address is below its owning module base",
        )
    })?;

    let remote_modules = snapshot_modules(target_pid, "remote_thread")?;
    let remote_owner = remote_modules
        .iter()
        .find(|module| module.module_key == owner.module_key)
        .ok_or_else(|| {
            InjectionError::failed(
                "REMOTE_SYSTEM_MODULE_NOT_FOUND",
                "remote_thread",
                format!("target does not contain '{}'", owner.module_key),
            )
        })?;
    if rva >= remote_owner.size {
        return Err(InjectionError::failed(
            "REMOTE_SYSTEM_MODULE_MISMATCH",
            "remote_thread",
            format!(
                "LoadLibraryW RVA 0x{rva:X} is outside target module '{}' (size 0x{:X})",
                remote_owner.module_key, remote_owner.size
            ),
        ));
    }
    let remote_address = remote_owner.base.checked_add(rva).ok_or_else(|| {
        InjectionError::failed(
            "REMOTE_LOAD_LIBRARY_OVERFLOW",
            "remote_thread",
            "target LoadLibraryW address overflowed",
        )
    })?;

    Ok(unsafe {
        std::mem::transmute::<usize, unsafe extern "system" fn(*mut c_void) -> u32>(remote_address)
    })
}

#[cfg(windows)]
fn defer_injection_cleanup(
    process: OwnedHandle,
    remote_thread: OwnedHandle,
    remote: RemoteAllocation,
) -> Result<(), InjectionError> {
    let process_value = process.value();
    let thread_value = remote_thread.value();
    let remote_address = remote.address;
    std::mem::forget(remote);
    std::mem::forget(remote_thread);
    std::mem::forget(process);

    let spawn_result = std::thread::Builder::new()
        .name("uexplorer-injection-cleanup".to_string())
        .spawn(move || cleanup_injection_resources(process_value, thread_value, remote_address));
    if let Err(error) = spawn_result {
        cleanup_injection_resources(process_value, thread_value, remote_address);
        return Err(InjectionError::failed(
            "CLEANUP_THREAD_CREATE_FAILED",
            "cleanup",
            format!("{error}; resources were cleaned synchronously after the remote thread exited"),
        ));
    }
    Ok(())
}

#[cfg(windows)]
fn cleanup_injection_resources(process: usize, remote_thread: usize, remote_address: usize) {
    let process_handle = HANDLE(process as *mut c_void);
    let thread_handle = HANDLE(remote_thread as *mut c_void);
    let wait_result = unsafe { WaitForSingleObject(thread_handle, INFINITE) };
    if wait_result == WAIT_OBJECT_0 {
        if let Err(error) = unsafe {
            VirtualFreeEx(
                process_handle,
                remote_address as *mut c_void,
                0,
                MEM_RELEASE,
            )
        } {
            log::error!(
                "REMOTE_FREE_FAILED in deferred cleanup: {}",
                windows_error_detail(&error)
            );
        }
    } else {
        log::error!(
            "REMOTE_CLEANUP_WAIT_FAILED: result=0x{:08X}; remote memory was retained for safety",
            wait_result.0
        );
    }
    unsafe {
        let _ = CloseHandle(thread_handle);
        let _ = CloseHandle(process_handle);
    }
}

#[cfg(windows)]
fn normalize_path_key(path: &str) -> String {
    let replaced = path.replace('/', "\\");
    let normalized = if let Some(rest) = replaced.strip_prefix("\\\\?\\UNC\\") {
        format!("\\\\{rest}")
    } else if let Some(rest) = replaced.strip_prefix("\\\\?\\") {
        rest.to_string()
    } else {
        replaced
    };
    normalized.trim_end_matches('\\').to_lowercase()
}

#[cfg(windows)]
fn io_error_detail(error: &std::io::Error) -> String {
    match error.raw_os_error() {
        Some(code) => format!("Win32={code}: {error}"),
        None => error.to_string(),
    }
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;

    fn amd64_pe_fixture() -> Vec<u8> {
        let mut bytes = vec![0u8; 0xA0];
        bytes[..2].copy_from_slice(b"MZ");
        bytes[0x3C..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        bytes[0x80..0x84].copy_from_slice(b"PE\0\0");
        bytes[0x84..0x86].copy_from_slice(&PE_MACHINE_AMD64.to_le_bytes());
        bytes[0x98..0x9A].copy_from_slice(&PE32_PLUS_MAGIC.to_le_bytes());
        bytes
    }

    #[test]
    fn accepts_amd64_pe32_plus_image() {
        assert_eq!(validate_amd64_dll_bytes(&amd64_pe_fixture()), Ok(()));
    }

    #[test]
    fn rejects_wrong_machine_and_truncated_headers() {
        let mut x86 = amd64_pe_fixture();
        x86[0x84..0x86].copy_from_slice(&0x014Cu16.to_le_bytes());
        assert_eq!(
            validate_amd64_dll_bytes(&x86),
            Err("PE machine is not AMD64")
        );

        let mut truncated = amd64_pe_fixture();
        truncated[0x3C..0x40].copy_from_slice(&0xFFFF_FFF0u32.to_le_bytes());
        assert_eq!(
            validate_amd64_dll_bytes(&truncated),
            Err("missing or truncated PE signature")
        );
    }

    #[test]
    fn normalizes_extended_windows_paths_without_losing_unc_identity() {
        assert_eq!(
            normalize_path_key(r"\\?\C:\Games\Core.DLL"),
            r"c:\games\core.dll"
        );
        assert_eq!(
            normalize_path_key(r"\\?\UNC\server\share\Core.dll"),
            r"\\server\share\core.dll"
        );
    }

    #[test]
    fn candidate_detection_returns_evidence_and_honors_exclusions() {
        let reasons = unreal_candidate_reasons(
            "Game-Win64-Shipping.exe",
            r"C:\Game\Binaries\Win64\Game.exe",
        );
        assert!(reasons
            .iter()
            .any(|reason| reason == "name:-win64-shipping"));
        assert!(unreal_candidate_reasons(
            "EpicGamesLauncher.exe",
            r"C:\Epic\Engine\Binaries\Win64\EpicGamesLauncher.exe"
        )
        .is_empty());
    }

    #[test]
    fn injection_result_serializes_independent_stage_states() {
        let result = injection_failure(
            "pipe_connect",
            "RPC_CONNECT_TIMEOUT",
            "fixture timeout",
            DllLoadState::Loaded,
            PipeConnectionState::Failed,
            CoreReadinessState::NotChecked,
        );
        let value = serde_json::to_value(result).unwrap();
        assert_eq!(value["success"], false);
        assert_eq!(value["status"], "failed");
        assert_eq!(value["dll"], "loaded");
        assert_eq!(value["pipe"], "failed");
        assert_eq!(value["core"], "not_checked");
        assert!(value["session"].is_null());
    }

    #[test]
    fn uncertain_remote_thread_outcomes_do_not_claim_the_dll_failed() {
        let timeout =
            InjectionError::failed("REMOTE_THREAD_TIMEOUT", "remote_wait", "fixture timeout");
        assert_eq!(timeout.dll_state_on_failure(), DllLoadState::Indeterminate);
        let cleanup = InjectionError::failed("REMOTE_FREE_FAILED", "cleanup", "fixture cleanup");
        assert_eq!(cleanup.dll_state_on_failure(), DllLoadState::Loaded);
        let rejected = InjectionError::failed(
            "LOAD_LIBRARY_RETURNED_NULL",
            "verify_load",
            "fixture rejection",
        );
        assert_eq!(rejected.dll_state_on_failure(), DllLoadState::Failed);
    }

    #[test]
    fn session_connection_failures_have_stable_stage_classification() {
        assert_eq!(
            classify_session_failure(&SessionManagerError::Client(
                CoreRpcClientError::ConnectTimeout { target_pid: 7 }
            )),
            (
                "pipe_connect",
                PipeConnectionState::Failed,
                CoreReadinessState::NotChecked
            )
        );
        assert_eq!(
            classify_session_failure(&SessionManagerError::Client(
                CoreRpcClientError::PeerPidMismatch {
                    expected: 7,
                    actual: 8
                }
            )),
            (
                "pipe_identity",
                PipeConnectionState::Rejected,
                CoreReadinessState::NotChecked
            )
        );
        assert_eq!(
            classify_session_failure(&SessionManagerError::Client(CoreRpcClientError::Session {
                code: "RPC_ENVELOPE_INVALID".to_string(),
                message: "fixture".to_string()
            })),
            (
                "core_ready",
                PipeConnectionState::Connected,
                CoreReadinessState::Failed
            )
        );
    }

    #[test]
    fn injection_admission_is_pid_scoped_and_released_by_raii() {
        let coordinator = Arc::new(TargetOperationCoordinator::default());
        let first = coordinator.reserve(701).unwrap();
        let other_pid = coordinator.reserve(702).unwrap();
        let duplicate = coordinator.reserve(701).unwrap_err();
        assert_eq!(duplicate.code, "TARGET_OPERATION_IN_PROGRESS");

        drop(first);
        let replacement = coordinator.reserve(701).unwrap();
        drop(replacement);
        drop(other_pid);
        assert!(coordinator.target_pids.lock().unwrap().is_empty());

        let reservations = (1..=MAX_CONCURRENT_TARGET_OPERATIONS as u32)
            .map(|pid| coordinator.reserve(pid).unwrap())
            .collect::<Vec<_>>();
        let limit = coordinator.reserve(999).unwrap_err();
        assert_eq!(limit.code, "TARGET_OPERATION_LIMIT_REACHED");
        drop(reservations);
        assert!(coordinator.target_pids.lock().unwrap().is_empty());
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let builder = tauri::Builder::default();
    #[cfg(windows)]
    let builder = builder
        .manage(Arc::new(SessionManager::new()))
        .manage(Arc::new(TargetOperationCoordinator::default()))
        .manage(Arc::new(EventBridgeManager::new()));

    builder
        .invoke_handler(tauri::generate_handler![
            scan_ue_processes,
            inject_and_connect,
            session_diagnostics,
            activate_session,
            subscribe_session_events,
            unsubscribe_session_events,
            event_bridge_diagnostics,
            disconnect_session,
            save_connection_settings,
        ])
        .setup(|app| {
            if cfg!(debug_assertions) {
                app.handle().plugin(
                    tauri_plugin_log::Builder::default()
                        .level(log::LevelFilter::Info)
                        .build(),
                )?;
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
