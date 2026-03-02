param(
    [Parameter(Mandatory=$true)]
    [int]$ProcessId,

    [Parameter(Mandatory=$true)]
    [string]$DllPath
)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public class NativeMethods {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out uint lpNumberOfBytesWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, out uint lpThreadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint ResumeThread(IntPtr hThread);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);
}
"@ -ErrorAction Stop

try {
    $PROCESS_ALL_ACCESS = 0x1F0FFF
    $MEM_COMMIT = 0x1000
    $MEM_RESERVE = 0x2000
    $PAGE_READWRITE = 0x04
    $CREATE_SUSPENDED = 0x04

    # Open process
    $hProcess = [NativeMethods]::OpenProcess($PROCESS_ALL_ACCESS, $false, $ProcessId)
    if ($hProcess -eq [IntPtr]::Zero) {
        throw "Failed to open process. Error: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    # Allocate memory for DLL path
    $dllBytes = [System.Text.Encoding]::Unicode.GetBytes($DllPath + [char]0)
    $hMem = [NativeMethods]::VirtualAllocEx($hProcess, [IntPtr]::Zero, $dllBytes.Length, ($MEM_COMMIT -bor $MEM_RESERVE), $PAGE_READWRITE)
    if ($hMem -eq [IntPtr]::Zero) {
        throw "Failed to allocate memory. Error: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    # Write DLL path to process memory
    $bytesWritten = 0
    $result = [NativeMethods]::WriteProcessMemory($hProcess, $hMem, $dllBytes, $dllBytes.Length, [ref]$bytesWritten)
    if (-not $result -or $bytesWritten -ne $dllBytes.Length) {
        throw "Failed to write memory. Error: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    # Get LoadLibraryW address
    $kernel32 = [System.Runtime.InteropServices.Marshal]::GetHINSTANCE([System.Reflection.Assembly]::GetAssembly([System.Object].GetType()).GetModule("kernel32.dll"))
    $LoadLibraryW = [System.Runtime.InteropServices.Marshal]::GetProcAddress($kernel32, "LoadLibraryW")

    # Create remote thread
    $threadId = 0
    $hThread = [NativeMethods]::CreateRemoteThread($hProcess, [IntPtr]::Zero, 0, $LoadLibraryW, $hMem, $CREATE_SUSPENDED, [ref]$threadId)
    if ($hThread -eq [IntPtr]::Zero) {
        throw "Failed to create remote thread. Error: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    # Resume the thread
    [NativeMethods]::ResumeThread($hThread) | Out-Null

    # Wait for completion
    [NativeMethods]::WaitForSingleObject($hThread, 10000) | Out-Null

    # Cleanup
    [NativeMethods]::CloseHandle($hThread) | Out-Null
    [NativeMethods]::CloseHandle($hProcess) | Out-Null

    Write-Output "SUCCESS: DLL injected"
    exit 0
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}
