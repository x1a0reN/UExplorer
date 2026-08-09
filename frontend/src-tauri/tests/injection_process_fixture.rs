#![cfg(windows)]

use app_lib::session::session_manager::SessionManager;
use app_lib::{
    inject_and_connect_target, CoreReadinessState, DllLoadState, InjectionStatus,
    InjectionTimeouts, PipeConnectionState,
};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};
use windows::core::PWSTR;
use windows::Win32::Foundation::{CloseHandle, FILETIME};
use windows::Win32::System::Threading::{
    GetProcessTimes, OpenProcess, QueryFullProcessImageNameW, PROCESS_NAME_WIN32,
    PROCESS_QUERY_LIMITED_INFORMATION,
};

const FIXTURE_WAIT: Duration = Duration::from_secs(5);

struct FixtureTarget {
    child: Child,
    stdin: Option<ChildStdin>,
    path: String,
    start_time_100ns: u64,
}

impl FixtureTarget {
    fn spawn(path: &Path) -> Self {
        assert!(
            path.is_file(),
            "missing injection target: {}",
            path.display()
        );
        let canonical = path.canonicalize().expect("canonicalize injection target");
        let mut child = Command::new(&canonical)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("launch injection target");
        let pid = child.id();
        let stdout = child.stdout.take().expect("capture target stdout");
        let (ready_tx, ready_rx) = mpsc::sync_channel(1);
        let ready_worker = thread::spawn(move || {
            let mut line = String::new();
            let result = BufReader::new(stdout)
                .read_line(&mut line)
                .map(|_| line.trim().to_string());
            let _ = ready_tx.send(result);
        });
        let ready = ready_rx
            .recv_timeout(FIXTURE_WAIT)
            .expect("injection target readiness timed out")
            .expect("read injection target readiness");
        ready_worker.join().expect("readiness worker panicked");
        assert_eq!(ready, "UEXPLORER_INJECTION_TARGET_READY");

        let (path, start_time_100ns) = query_identity(pid);
        assert_eq!(
            normalize_path(&path),
            normalize_path(&canonical.to_string_lossy())
        );
        Self {
            stdin: child.stdin.take(),
            child,
            path,
            start_time_100ns,
        }
    }

    fn pid(&self) -> u32 {
        self.child.id()
    }

    fn stop(&mut self) {
        if let Some(mut stdin) = self.stdin.take() {
            stdin.write_all(b"exit\n").expect("signal target exit");
        }
        let deadline = Instant::now() + FIXTURE_WAIT;
        loop {
            if let Some(status) = self.child.try_wait().expect("poll injection target") {
                assert!(status.success(), "injection target exit failed: {status}");
                return;
            }
            if Instant::now() >= deadline {
                self.child.kill().expect("terminate stuck injection target");
                let _ = self.child.wait();
                panic!("injection target did not exit within {FIXTURE_WAIT:?}");
            }
            thread::sleep(Duration::from_millis(10));
        }
    }
}

impl Drop for FixtureTarget {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_some() {
            return;
        }
        if let Some(mut stdin) = self.stdin.take() {
            let _ = stdin.write_all(b"exit\n");
        }
        let deadline = Instant::now() + Duration::from_secs(1);
        while Instant::now() < deadline {
            if self.child.try_wait().ok().flatten().is_some() {
                return;
            }
            thread::sleep(Duration::from_millis(10));
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("src-tauri must be nested under the repository root")
        .to_path_buf()
}

fn artifact(relative: &str) -> PathBuf {
    let path = repo_root().join(relative);
    assert!(
        path.is_file(),
        "missing required fixture: {}",
        path.display()
    );
    path
}

fn query_identity(pid: u32) -> (String, u64) {
    let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) }
        .expect("open injection target for identity");
    let mut path = vec![0u16; 32_768];
    let mut path_len = path.len() as u32;
    unsafe {
        QueryFullProcessImageNameW(
            process,
            PROCESS_NAME_WIN32,
            PWSTR(path.as_mut_ptr()),
            &mut path_len,
        )
    }
    .expect("query injection target path");

    let mut creation = FILETIME::default();
    let mut exit = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    unsafe { GetProcessTimes(process, &mut creation, &mut exit, &mut kernel, &mut user) }
        .expect("query injection target start time");
    unsafe { CloseHandle(process) }.expect("close injection target query handle");
    (
        String::from_utf16_lossy(&path[..path_len as usize]),
        ((creation.dwHighDateTime as u64) << 32) | creation.dwLowDateTime as u64,
    )
}

fn normalize_path(path: &str) -> String {
    let replaced = path.replace('/', "\\");
    replaced
        .strip_prefix(r"\\?\")
        .unwrap_or(&replaced)
        .trim_end_matches('\\')
        .to_lowercase()
}

fn invoke(
    manager: &SessionManager,
    target: &FixtureTarget,
    dll: &Path,
    timeouts: InjectionTimeouts,
) -> app_lib::InjectionResult {
    inject_and_connect_target(
        manager,
        target.pid(),
        &dll.to_string_lossy(),
        &target.start_time_100ns.to_string(),
        &target.path,
        timeouts,
    )
}

fn fixture_timeouts() -> InjectionTimeouts {
    InjectionTimeouts {
        remote_thread_ms: 5_000,
        core_connect: FIXTURE_WAIT,
    }
}

#[test]
fn production_injector_handles_live_success_and_failure_matrix() {
    let target_x64 = artifact("tests/injection-fixture/x64/Release/InjectionTarget.exe");
    let target_x86 = artifact("tests/injection-fixture/Win32/Release/InjectionTarget.exe");
    let ready_x64 = artifact("tests/injection-fixture/x64/Release/InjectionCoreFixture.dll");
    let ready_x86 = artifact("tests/injection-fixture/Win32/Release/InjectionCoreFixture.dll");
    let slow_x64 = artifact("tests/injection-fixture/x64/Slow/InjectionSlowFixture.dll");
    let reject_x64 = artifact("tests/injection-fixture/x64/Reject/InjectionRejectFixture.dll");

    let mut success_target = FixtureTarget::spawn(&target_x64);
    let success_manager = SessionManager::new();
    let success = invoke(
        &success_manager,
        &success_target,
        &ready_x64,
        fixture_timeouts(),
    );
    assert!(
        success.success,
        "successful injection failed: {}",
        success.message
    );
    assert_eq!(success.status, InjectionStatus::Ready);
    assert_eq!(success.code, "CORE_READY");
    assert_eq!(success.dll, DllLoadState::Loaded);
    assert_eq!(success.pipe, PipeConnectionState::Connected);
    assert_eq!(success.core, CoreReadinessState::Ready);
    let session = success
        .session
        .expect("successful injection omitted session");
    assert_eq!(session.target_pid, success_target.pid());
    assert_eq!(session.server_pid, success_target.pid());
    assert_eq!(session.core_version, "injection-fixture-1.0");
    assert_eq!(
        session.capabilities.get("transport.named_pipe"),
        Some(&true)
    );

    let duplicate = invoke(
        &success_manager,
        &success_target,
        &ready_x64,
        fixture_timeouts(),
    );
    assert!(duplicate.success, "already-loaded reconnect was not ready");
    assert_eq!(duplicate.code, "CORE_ALREADY_READY");
    assert_eq!(duplicate.dll, DllLoadState::AlreadyLoaded);
    success_manager
        .disconnect(success_target.pid(), "fixture_complete", FIXTURE_WAIT)
        .expect("shutdown injected Core fixture");
    success_target.stop();

    let mut stale_target = FixtureTarget::spawn(&target_x64);
    let stale_manager = SessionManager::new();
    let stale = inject_and_connect_target(
        &stale_manager,
        stale_target.pid(),
        &ready_x64.to_string_lossy(),
        &(stale_target.start_time_100ns + 1).to_string(),
        &stale_target.path,
        fixture_timeouts(),
    );
    assert!(!stale.success);
    assert_eq!(stale.code, "PROCESS_IDENTITY_MISMATCH");
    assert_eq!(stale.dll, DllLoadState::NotAttempted);
    stale_target.stop();

    let mut wrong_dll_target = FixtureTarget::spawn(&target_x64);
    let wrong_dll = invoke(
        &SessionManager::new(),
        &wrong_dll_target,
        &ready_x86,
        fixture_timeouts(),
    );
    assert!(!wrong_dll.success);
    assert_eq!(wrong_dll.code, "DLL_PE_INVALID");
    assert_eq!(wrong_dll.dll, DllLoadState::NotAttempted);
    wrong_dll_target.stop();

    let mut wrong_target = FixtureTarget::spawn(&target_x86);
    let wrong_arch = invoke(
        &SessionManager::new(),
        &wrong_target,
        &ready_x64,
        fixture_timeouts(),
    );
    assert!(!wrong_arch.success);
    assert_eq!(wrong_arch.code, "TARGET_ARCH_MISMATCH");
    assert_eq!(wrong_arch.dll, DllLoadState::NotAttempted);
    wrong_target.stop();

    let mut reject_target = FixtureTarget::spawn(&target_x64);
    let rejected = invoke(
        &SessionManager::new(),
        &reject_target,
        &reject_x64,
        fixture_timeouts(),
    );
    assert!(!rejected.success);
    assert_eq!(rejected.code, "LOAD_LIBRARY_RETURNED_NULL");
    assert_eq!(rejected.dll, DllLoadState::Failed);
    assert_eq!(rejected.pipe, PipeConnectionState::NotAttempted);
    reject_target.stop();

    let mut slow_target = FixtureTarget::spawn(&target_x64);
    let slow_manager = SessionManager::new();
    let timed_out = invoke(
        &slow_manager,
        &slow_target,
        &slow_x64,
        InjectionTimeouts {
            remote_thread_ms: 100,
            core_connect: FIXTURE_WAIT,
        },
    );
    assert!(!timed_out.success);
    assert_eq!(timed_out.code, "REMOTE_THREAD_TIMEOUT");
    assert_eq!(timed_out.dll, DllLoadState::Indeterminate);
    thread::sleep(Duration::from_millis(900));

    let recovered = invoke(&slow_manager, &slow_target, &slow_x64, fixture_timeouts());
    assert!(
        recovered.success,
        "timed-out load did not become reconnectable"
    );
    assert_eq!(recovered.dll, DllLoadState::AlreadyLoaded);
    assert_eq!(recovered.core, CoreReadinessState::Ready);
    slow_manager
        .disconnect(slow_target.pid(), "timeout_fixture_complete", FIXTURE_WAIT)
        .expect("shutdown slow injected Core fixture");
    slow_target.stop();
}
