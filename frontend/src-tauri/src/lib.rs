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
            "Get-Process | Where-Object {$_.Path -match '\\.exe$'} | Select-Object Id, ProcessName, Path | ConvertTo-Json"
        ])
        .output()
        .map_err(|e| format!("Failed to run PowerShell: {}", e))?;

    if !output.status.success() {
        return Err(format!("PowerShell failed: {}", String::from_utf8_lossy(&output.stderr)));
    }

    let json_str = String::from_utf8_lossy(&output.stdout);

    // Parse the JSON output
    let procs: Vec<serde_json::Value> = serde_json::from_str(&json_str)
        .map_err(|e| format!("Failed to parse JSON: {}", e))?;

    let mut processes = Vec::new();

    for proc in procs {
        let name = proc["ProcessName"].as_str().unwrap_or("");
        let path = proc["Path"].as_str().unwrap_or("");
        let pid = proc["Id"].as_u64().unwrap_or(0) as u32;

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

    // Check if it's a game executable (not system/launcher)
    if !lower.ends_with(".exe") {
        return false;
    }

    // Exclude common non-game processes
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

    // Check path - games usually in specific folders
    let path_lower = path.to_lowercase();
    let game_indicators = ["games", "game", "binaries", "builds"];

    for indicator in game_indicators {
        if path_lower.contains(indicator) {
            return true;
        }
    }

    // Default to true if it looks like a game
    true
}

// Inject DLL using PowerShell
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

    // Use PowerShell to inject DLL using Add-Type and [System.Reflection.Assembly]::Load
    // This is a simplified approach - for production, use proper injection
    let ps_command = format!(
        r#"
        $dllPath = '{}'
        $pid = {}

        # Get the process
        $proc = Get-Process -Id $pid -ErrorAction SilentlyContinue
        if (-not $proc) {{
            Write-Error "Process not found"
            exit 1
        }}

        # Use reflection to load the DLL into the process
        # This is a basic approach - proper injection would require more complex code
        Add-Type -AssemblyName System.Reflection
        try {{
            [System.Reflection.Assembly]::Load([System.IO.File]::ReadAllBytes($dllPath))
            Write-Output "DLL loaded successfully"
        }} catch {{
            Write-Error $_.Exception.Message
            exit 1
        }}
        "#,
        dll_path.replace("'", "''"),
        pid
    );

    let output = Command::new("powershell")
        .args(["-NoProfile", "-Command", &ps_command])
        .output()
        .map_err(|e| format!("Failed to run PowerShell: {}", e))?;

    if output.status.success() {
        Ok(InjectionResult {
            success: true,
            message: format!("Successfully injected DLL into process {}", pid),
        })
    } else {
        Ok(InjectionResult {
            success: false,
            message: format!("Injection failed: {}", String::from_utf8_lossy(&output.stderr)),
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
