use serde::{Deserialize, Serialize};
use tauri::command;

#[cfg(windows)]
use windows::{
    core::PCWSTR,
    Win32::Foundation::{CloseHandle, HANDLE, PROCESS_ALL_ACCESS},
    Win32::System::Threading::{
        OpenProcess, VirtualAllocEx, WriteProcessMemory, CreateRemoteThread,
    },
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

// Scan all running processes and filter for Unreal Engine processes
#[command]
pub fn scan_ue_processes() -> Result<Vec<ProcessInfo>, String> {
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
    use windows::Win32::System::ProcessStatus::EnumProcesses;
    use windows::Win32::System::Threading::QueryFullProcessImageNameW;
    use std::ffi::OsString;
    use std::os::windows::ffi::OsStringExt;

    let mut processes = Vec::new();
    let mut bytes_returned = 0u32;
    let mut pids = vec![0u32; 1024];

    unsafe {
        // First call to get required size
        let _ = EnumProcesses(
            Some(&mut pids),
            (pids.len() * std::mem::size_of::<u32>()) as u32,
            Some(&mut bytes_returned),
        );

        let num_processes = bytes_returned as usize / std::mem::size_of::<u32>();
        pids.truncate(num_processes);

        for &pid in &pids {
            if pid == 0 {
                continue;
            }

            // Try to open the process
            if let Ok(handle) = OpenProcess(PROCESS_ALL_ACCESS, false, pid) {
                // Get process name using QueryFullProcessImageNameW
                let mut name_buf = vec![0u16; 260];
                let mut size = name_buf.len() as u32;

                if QueryFullProcessImageNameW(
                    handle,
                    windows::Win32::System::Threading::PROCESS_NAME_FORMAT(0),
                    windows::core::PWSTR(name_buf.as_mut_ptr()),
                    &mut size,
                ).is_ok() && size > 0 {
                    let path = OsString::from_wide(&name_buf[..size as usize])
                        .to_string_lossy()
                        .to_string();

                    // Get just the filename
                    let name = std::path::Path::new(&path)
                        .file_name()
                        .map(|n| n.to_string_lossy().to_string())
                        .unwrap_or_default();

                    // Filter for Unreal Engine processes
                    // Common UE process names: UE4Game, UE5Game, GameName.exe, etc.
                    // Also check for common game engines
                    let is_ue = name.to_lowercase().contains("ue4")
                        || name.to_lowercase().contains("ue5")
                        || name.to_lowercase().contains("unreal")
                        || is_likely_game_process(&name, &path);

                    if is_ue {
                        processes.push(ProcessInfo {
                            pid,
                            name,
                            path,
                        });
                    }
                }

                let _ = CloseHandle(handle);
            }
        }
    }

    // Sort by name
    processes.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));

    Ok(processes)
}

#[cfg(windows)]
fn is_likely_game_process(name: &str, _path: &str) -> bool {
    // Additional heuristics to detect game processes
    let lower = name.to_lowercase();

    // Common game suffixes
    lower.ends_with(".exe")
        && !lower.contains("steam")
        && !lower.contains("epic")
        && !lower.contains("launcher")
        && !lower.contains("updater")
        && !lower.contains("helper")
        && !lower.contains("service")
        && !lower.contains("renderer")
        && !lower.contains("crashreporter")
}

// Inject DLL into a process
#[command]
pub fn inject_dll(pid: u32, dll_path: String) -> Result<InjectionResult, String> {
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
    use windows::Win32::Foundation::CloseHandle;
    use std::ptr::null_mut;

    // Validate DLL path
    if !std::path::Path::new(&dll_path).exists() {
        return Ok(InjectionResult {
            success: false,
            message: format!("DLL not found: {}", dll_path),
        });
    }

    // Convert DLL path to wide string
    let dll_path_wide: Vec<u16> = dll_path.encode_utf16().chain(std::iter::once(0)).collect();

    unsafe {
        // Open the target process
        let handle = match OpenProcess(PROCESS_ALL_ACCESS, false, pid) {
            Ok(h) => h,
            Err(e) => {
                return Ok(InjectionResult {
                    success: false,
                    message: format!("Failed to open process {}: {:?}", pid, e),
                });
            }
        };

        // Allocate memory in the target process for DLL path
        let remote_addr = VirtualAllocEx(
            handle,
            None,
            dll_path_wide.len() * std::mem::size_of::<u16>(),
            windows::Win32::Memory::MEM_COMMIT | windows::Win32::Memory::MEM_RESERVE,
            windows::Win32::Memory::PAGE_READWRITE,
        );

        if remote_addr.is_null() {
            let _ = CloseHandle(handle);
            return Ok(InjectionResult {
                success: false,
                message: "Failed to allocate memory in target process".to_string(),
            });
        }

        // Write DLL path to allocated memory
        let mut bytes_written = 0usize;
        if WriteProcessMemory(
            handle,
            remote_addr,
            dll_path_wide.as_ptr() as *const _,
            dll_path_wide.len() * std::mem::size_of::<u16>(),
            Some(&mut bytes_written),
        ).is_err() {
            let _ = CloseHandle(handle);
            return Ok(InjectionResult {
                success: false,
                message: "Failed to write DLL path to target process".to_string(),
            });
        }

        // Get LoadLibraryW address
        let kernel32 = windows::Win32::System::LibraryLoader::GetModuleHandleW(
            windows::core::PCWSTR::from_raw("kernel32\0".as_ptr())
        ).expect("Failed to get kernel32 handle");

        let load_library = windows::Win32::System::LibraryLoader::GetProcAddress(
            kernel32,
            windows::core::PCSTR::from_raw("LoadLibraryW\0".as_ptr())
        ).expect("Failed to get LoadLibraryW address") as *mut _;

        // Create remote thread to load DLL
        let mut thread_handle = HANDLE::default();
        if CreateRemoteThread(
            handle,
            None,
            windows::Win32::System::Threading::DEFAULT_THREAD_STACK_SIZE,
            Some(std::mem::transmute(load_library)),
            Some(remote_addr),
            windows::Win32::System::Threading::CREATE_SUSPENDED,
            Some(&mut 0u32),
        ).is_err() {
            let _ = CloseHandle(handle);
            return Ok(InjectionResult {
                success: false,
                message: "Failed to create remote thread".to_string(),
            });
        }

        // Resume the thread (it starts suspended)
        windows::Win32::System::Threading::ResumeThread(thread_handle);

        // Wait for the thread to complete
        windows::Win32::System::Threading::WaitForSingleObject(thread_handle, 10000);

        let _ = CloseHandle(thread_handle);
        let _ = CloseHandle(handle);

        Ok(InjectionResult {
            success: true,
            message: format!("Successfully injected DLL into process {}", pid),
        })
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            scan_ue_processes,
            inject_dll,
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
