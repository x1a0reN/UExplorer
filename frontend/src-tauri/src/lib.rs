use serde::{Deserialize, Serialize};
use std::fs;
use std::net::{SocketAddr, TcpStream};
use std::path::PathBuf;
use std::time::Duration;
use tauri::command;
use tauri::Manager;

#[cfg(windows)]
use windows::core::{PCSTR, PCWSTR, PWSTR};
#[cfg(windows)]
use windows::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
#[cfg(windows)]
use windows::Win32::System::Diagnostics::Debug::WriteProcessMemory;
#[cfg(windows)]
use windows::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
};
#[cfg(windows)]
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
#[cfg(windows)]
use windows::Win32::System::Memory::{
    VirtualAllocEx, VirtualFreeEx, MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE,
};
#[cfg(windows)]
use windows::Win32::System::Threading::{
    CreateRemoteThread, OpenProcess, QueryFullProcessImageNameW, ResumeThread, WaitForSingleObject,
    CREATE_SUSPENDED, PROCESS_ALL_ACCESS, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION,
};

// Process info structure
#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct ProcessInfo {
    pub pid: u32,
    pub name: String,
    pub path: String,
}

// Injection result
#[derive(Debug, Serialize, Deserialize)]
pub struct InjectionResult {
    pub success: bool,
    pub message: String,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct RuntimeEndpoint {
    pub pid: u32,
    pub port: u16,
    pub token: String,
    pub running: bool,
}

fn uexplorer_data_dir() -> Result<PathBuf, String> {
    let base = std::env::var("LOCALAPPDATA")
        .map(PathBuf::from)
        .or_else(|_| std::env::current_dir().map(|p| p.join("temp")))
        .map_err(|e| format!("Cannot resolve data dir: {e}"))?;
    let dir = base.join("UExplorer");
    fs::create_dir_all(&dir).map_err(|e| format!("Create data dir failed: {e}"))?;
    Ok(dir)
}

fn connection_ini_path() -> Result<PathBuf, String> {
    Ok(uexplorer_data_dir()?.join("connection.ini"))
}

fn runtime_ini_path() -> Result<PathBuf, String> {
    Ok(uexplorer_data_dir()?.join("runtime.ini"))
}

fn read_ini_value(content: &str, section: &str, key: &str) -> Option<String> {
    let mut current_section = String::new();
    for raw_line in content.lines() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with(';') || line.starts_with('#') {
            continue;
        }

        if line.starts_with('[') && line.ends_with(']') && line.len() >= 3 {
            current_section = line[1..line.len() - 1].trim().to_string();
            continue;
        }

        if !current_section.eq_ignore_ascii_case(section) {
            continue;
        }

        if let Some(idx) = line.find('=') {
            let k = line[..idx].trim();
            if k.eq_ignore_ascii_case(key) {
                return Some(line[idx + 1..].trim().to_string());
            }
        }
    }
    None
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

#[command]
fn load_runtime_endpoint() -> Result<Option<RuntimeEndpoint>, String> {
    let path = runtime_ini_path()?;
    if !path.exists() {
        return Ok(None);
    }

    let content = fs::read_to_string(&path).map_err(|e| format!("Read runtime state failed: {e}"))?;
    let pid = read_ini_value(&content, "Runtime", "Pid")
        .and_then(|v| v.parse::<u32>().ok())
        .unwrap_or(0);
    let port = read_ini_value(&content, "Runtime", "Port")
        .and_then(|v| v.parse::<u16>().ok())
        .unwrap_or(0);
    let token = read_ini_value(&content, "Runtime", "Token").unwrap_or_default();
    let running = read_ini_value(&content, "Runtime", "Running")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false);

    if port == 0 || token.is_empty() {
        return Ok(None);
    }

    Ok(Some(RuntimeEndpoint {
        pid,
        port,
        token,
        running,
    }))
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
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
            .map_err(|e| format!("CreateToolhelp32Snapshot failed: {e}"))?;

        let mut entry = PROCESSENTRY32W::default();
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;

        if Process32FirstW(snapshot, &mut entry).is_ok() {
            loop {
                let pid = entry.th32ProcessID;
                if pid > 0 {
                    let name = utf16_z_to_string(&entry.szExeFile);
                    let path = query_process_path(pid).unwrap_or_default();
                    let name_lower = name.to_lowercase();

                    let is_ue = name_lower.contains("ue4")
                        || name_lower.contains("ue5")
                        || name_lower.contains("unreal")
                        || is_likely_game_process(&name, &path);

                    if is_ue {
                        processes.push(ProcessInfo { pid, name, path });
                    }
                }

                if Process32NextW(snapshot, &mut entry).is_err() {
                    break;
                }
            }
        }

        let _ = CloseHandle(snapshot);
    }

    // Sort by name
    processes.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));

    Ok(processes)
}

#[cfg(windows)]
fn utf16_z_to_string(buf: &[u16]) -> String {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..end])
}

#[cfg(windows)]
fn query_process_path(pid: u32) -> Option<String> {
    unsafe {
        let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid).ok()?;
        let mut path_buf = vec![0u16; 32768];
        let mut size = path_buf.len() as u32;

        let ok = QueryFullProcessImageNameW(
            handle,
            PROCESS_NAME_WIN32,
            PWSTR(path_buf.as_mut_ptr()),
            &mut size,
        )
        .is_ok();

        let _ = CloseHandle(handle);
        if !ok || size == 0 {
            return None;
        }

        Some(String::from_utf16_lossy(&path_buf[..size as usize]))
    }
}

#[cfg(windows)]
fn is_likely_game_process(name: &str, path: &str) -> bool {
    let lower = name.to_lowercase();
    let path_lower = path.to_lowercase();
    let excludes = [
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
    ];

    for ex in excludes {
        if lower.contains(ex) || path_lower.contains(ex) {
            return false;
        }
    }

    let game_indicators = ["games", "game", "binaries", "builds"];

    for indicator in game_indicators {
        if path_lower.contains(indicator) {
            return true;
        }
    }

    true
}

// DLL Injection using CreateRemoteThread via Windows API
#[command]
fn inject_dll(pid: u32, dll_path: String) -> Result<InjectionResult, String> {
    #[cfg(windows)]
    {
        inject_dll_internal(pid, dll_path)
    }
    #[cfg(not(windows))]
    {
        Err("DLL injection is only supported on Windows".to_string())
    }
}

#[cfg(windows)]
fn inject_dll_internal(pid: u32, dll_path: String) -> Result<InjectionResult, String> {
    // Validate DLL path
    if !std::path::Path::new(&dll_path).exists() {
        return Ok(InjectionResult {
            success: false,
            message: format!("DLL not found: {}", dll_path),
        });
    }

    // Convert DLL path to wide string (UTF-16)
    let dll_path_wide: Vec<u16> = dll_path.encode_utf16().chain(std::iter::once(0)).collect();

    unsafe {
        let h_process = match OpenProcess(PROCESS_ALL_ACCESS, false, pid) {
            Ok(handle) => handle,
            Err(e) => {
                return Ok(InjectionResult {
                    success: false,
                    message: format!("Failed to open process (PID: {}): {}", pid, e),
                });
            }
        };

        // Allocate memory in target process for DLL path
        let mem_size = dll_path_wide.len() * std::mem::size_of::<u16>();
        let remote_mem = VirtualAllocEx(
            h_process,
            None,
            mem_size,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE,
        );
        if remote_mem.is_null() {
            let _ = CloseHandle(h_process);
            return Ok(InjectionResult {
                success: false,
                message: "Failed to allocate memory in target process".to_string(),
            });
        }

        // Write DLL path to allocated memory
        let mut bytes_written = 0usize;
        let write_result = WriteProcessMemory(
            h_process,
            remote_mem,
            dll_path_wide.as_ptr() as *const _,
            mem_size,
            Some(&mut bytes_written),
        );

        if write_result.is_err() || bytes_written != mem_size {
            let _ = VirtualFreeEx(h_process, remote_mem, 0, MEM_RELEASE);
            let _ = CloseHandle(h_process);
            return Ok(InjectionResult {
                success: false,
                message: "Failed to write DLL path to target process".to_string(),
            });
        }

        // Get LoadLibraryW address from kernel32.dll
        let kernel32_name: Vec<u16> = "kernel32.dll\0".encode_utf16().collect();
        let kernel32 = match GetModuleHandleW(PCWSTR(kernel32_name.as_ptr())) {
            Ok(module) => module,
            Err(_) => {
                let _ = VirtualFreeEx(h_process, remote_mem, 0, MEM_RELEASE);
                let _ = CloseHandle(h_process);
                return Ok(InjectionResult {
                    success: false,
                    message: "Failed to get kernel32 handle".to_string(),
                });
            }
        };

        let load_library = GetProcAddress(kernel32, PCSTR(c"LoadLibraryW".as_ptr() as *const u8));
        let Some(load_library) = load_library else {
            let _ = VirtualFreeEx(h_process, remote_mem, 0, MEM_RELEASE);
            let _ = CloseHandle(h_process);
            return Ok(InjectionResult {
                success: false,
                message: "Failed to get LoadLibraryW address".to_string(),
            });
        };

        let start_routine = std::mem::transmute(load_library);

        // Create remote thread
        let h_thread = match CreateRemoteThread(
            h_process,
            None,
            0,
            Some(start_routine),
            Some(remote_mem),
            CREATE_SUSPENDED.0,
            None,
        ) {
            Ok(handle) => handle,
            Err(_) => {
                let _ = VirtualFreeEx(h_process, remote_mem, 0, MEM_RELEASE);
                let _ = CloseHandle(h_process);
                return Ok(InjectionResult {
                    success: false,
                    message: "Failed to create remote thread".to_string(),
                });
            }
        };

        // Resume the thread to start execution
        let _ = ResumeThread(h_thread);

        // Wait for thread to complete (10 second timeout)
        let wait_result = WaitForSingleObject(h_thread, 10000);

        // Cleanup
        let _ = CloseHandle(h_thread);
        let _ = VirtualFreeEx(h_process, remote_mem, 0, MEM_RELEASE);
        let _ = CloseHandle(h_process);

        if wait_result == WAIT_OBJECT_0 {
            Ok(InjectionResult {
                success: true,
                message: format!("Successfully injected DLL into process {}", pid),
            })
        } else {
            Ok(InjectionResult {
                success: false,
                message: "Injection timeout - DLL may not have loaded".to_string(),
            })
        }
    }
}

#[cfg(not(windows))]
fn inject_dll_internal(_pid: u32, _dll_path: String) -> Result<InjectionResult, String> {
    Err("DLL injection is only supported on Windows".to_string())
}

fn is_dev_server_running() -> bool {
    let addr: SocketAddr = match "127.0.0.1:5173".parse() {
        Ok(addr) => addr,
        Err(_) => return false,
    };
    TcpStream::connect_timeout(&addr, Duration::from_millis(500)).is_ok()
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            scan_ue_processes,
            inject_dll,
            save_connection_settings,
            load_runtime_endpoint,
        ])
        .setup(|app| {
            if cfg!(debug_assertions) {
                // Debug app.exe depends on devUrl; if local Vite is not running,
                // fallback to embedded assets to avoid browser error page.
                if !is_dev_server_running() {
                    if let Some(window) = app.get_webview_window("main") {
                        if let Ok(url) = tauri::Url::parse("tauri://localhost/index.html") {
                            let _ = window.navigate(url);
                        }
                    }
                }

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
