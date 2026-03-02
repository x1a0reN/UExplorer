use serde::{Deserialize, Serialize};
use tauri::command;
use tauri::Manager;
use std::net::{SocketAddr, TcpStream};
use std::time::Duration;

#[cfg(windows)]
use std::process::Command;
#[cfg(windows)]
use windows::core::{PCSTR, PCWSTR};
#[cfg(windows)]
use windows::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
#[cfg(windows)]
use windows::Win32::System::Diagnostics::Debug::WriteProcessMemory;
#[cfg(windows)]
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
#[cfg(windows)]
use windows::Win32::System::Memory::{
    VirtualAllocEx, VirtualFreeEx, MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE,
};
#[cfg(windows)]
use windows::Win32::System::Threading::{
    CreateRemoteThread, OpenProcess, ResumeThread, WaitForSingleObject, CREATE_SUSPENDED,
    PROCESS_ALL_ACCESS,
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

// Scan all running processes using PowerShell and filter for Unreal Engine processes
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
    // Use PowerShell to get process list with paths
    let output = Command::new("powershell")
        .args([
            "-NoProfile",
            "-Command",
            "Get-Process | Where-Object {$_.Path -match '\\.exe$'} | Select-Object Id, ProcessName, Path | ConvertTo-Json -Compress"
        ])
        .output()
        .map_err(|e| format!("Failed to run PowerShell: {}", e))?;

    if !output.status.success() {
        return Err(format!(
            "PowerShell failed: {}",
            String::from_utf8_lossy(&output.stderr)
        ));
    }

    let json_str = String::from_utf8_lossy(&output.stdout);

    // Handle both single object and array
    let procs: Vec<serde_json::Value> = if json_str.trim().starts_with('[') {
        serde_json::from_str(&json_str).map_err(|e| format!("Failed to parse JSON: {}", e))?
    } else if json_str.trim().starts_with('{') {
        vec![serde_json::from_str(&json_str).map_err(|e| format!("Failed to parse JSON: {}", e))?]
    } else {
        return Ok(Vec::new());
    };

    let mut processes = Vec::new();

    for proc in procs {
        let name = proc
            .get("ProcessName")
            .and_then(|v| v.as_str())
            .unwrap_or("");
        let path = proc.get("Path").and_then(|v| v.as_str()).unwrap_or("");
        let pid = proc.get("Id").and_then(|v| v.as_u64()).unwrap_or(0) as u32;

        let name_lower = name.to_lowercase();

        // Filter for Unreal Engine processes
        let is_ue = name_lower.contains("ue4")
            || name_lower.contains("ue5")
            || name_lower.contains("unreal")
            || is_likely_game_process(name, path);

        if is_ue && pid > 0 {
            processes.push(ProcessInfo {
                pid,
                name: name.to_string(),
                path: path.to_string(),
            });
        }
    }

    // Sort by name
    processes.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));

    Ok(processes)
}

#[cfg(windows)]
fn is_likely_game_process(name: &str, path: &str) -> bool {
    let lower = name.to_lowercase();

    if !lower.ends_with(".exe") {
        return false;
    }

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
        if lower.contains(ex) || path.to_lowercase().contains(ex) {
            return false;
        }
    }

    let path_lower = path.to_lowercase();
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
        .invoke_handler(tauri::generate_handler![scan_ue_processes, inject_dll,])
        .setup(|app| {
            // If local Vite dev server is unavailable, force fallback to embedded assets.
            if !is_dev_server_running() {
                if let Some(window) = app.get_webview_window("main") {
                    if let Ok(url) = tauri::Url::parse("tauri://localhost/") {
                        let _ = window.navigate(url);
                    }
                }
            }

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
