use serde::{Deserialize, Serialize};
use tauri::command;

#[cfg(windows)]
use std::process::Command;

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
        return Err(format!("PowerShell failed: {}", String::from_utf8_lossy(&output.stderr)));
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
        let name = proc.get("ProcessName").and_then(|v| v.as_str()).unwrap_or("");
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
        "steam", "epic", "launcher", "updater", "helper",
        "service", "renderer", "crashreporter", "ue4prereq",
        "vc redist", "vcredist", "directx", ".net",
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

// DLL Injection using CreateRemoteThread via PowerShell
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

    // Build PowerShell script for remote thread injection
    let ps_script = include_str!("inject_dll.ps1")
        .replace("{PID}", &pid.to_string())
        .replace("{DLL_PATH}", &dll_path);

    let output = Command::new("powershell")
        .args([
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-Command", &ps_script
        ])
        .output()
        .map_err(|e| format!("Failed to run PowerShell: {}", e))?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    if output.status.success() && stdout.contains("SUCCESS") {
        Ok(InjectionResult {
            success: true,
            message: format!("Successfully injected DLL into process {}", pid),
        })
    } else {
        Ok(InjectionResult {
            success: false,
            message: format!("Injection failed: {} {}", stdout.trim(), stderr.trim()),
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
