#![cfg(windows)]

use app_lib::session::event_hub::EventFilter;
use app_lib::session::session_manager::{SessionManager, SessionPhase, TargetProcessIdentity};
use app_lib::session::snapshot_cache::{SnapshotObjectKind, SnapshotQuery};
use serde_json::Value;
use std::io::{BufRead, BufReader, Read};
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStderr, ChildStdout, Command, ExitStatus, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{CloseHandle, FILETIME};
use windows::Win32::System::Threading::{
    GetProcessTimes, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
};

const READY_PREFIX: &str = "UEXPLORER_HOST_FIXTURE_READY ";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

struct FixtureProcess {
    child: Child,
    completed: bool,
}

impl FixtureProcess {
    fn spawn(executable: &Path) -> Self {
        let child = Command::new(executable)
            .arg("--host-session-fixture")
            .creation_flags(CREATE_NO_WINDOW)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap_or_else(|error| {
                panic!(
                    "failed to start required C++ Core fixture {}: {error}",
                    executable.display()
                )
            });
        Self {
            child,
            completed: false,
        }
    }

    fn finish(&mut self, timeout: Duration) -> ExitStatus {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(status) = self.child.try_wait().expect("fixture process wait failed") {
                self.completed = true;
                return status;
            }
            assert!(
                Instant::now() < deadline,
                "C++ Core fixture did not exit after exact Host Shutdown"
            );
            thread::sleep(Duration::from_millis(10));
        }
    }
}

impl Drop for FixtureProcess {
    fn drop(&mut self) {
        if !self.completed {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

fn fixture_executable() -> PathBuf {
    let repository = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
        .canonicalize()
        .expect("repository root is unavailable");
    let executable = repository
        .join("tests")
        .join("core-harness")
        .join("x64")
        .join("Release")
        .join("CoreHarness.exe");
    assert!(
        executable.is_file(),
        "required C++ Core fixture is missing: {}",
        executable.display()
    );
    executable
        .canonicalize()
        .expect("C++ Core fixture path cannot be canonicalized")
}

fn read_stdout(stdout: ChildStdout) -> (Receiver<String>, JoinHandle<Result<Vec<String>, String>>) {
    let (sender, receiver) = mpsc::channel();
    let worker = thread::spawn(move || {
        let mut captured = Vec::new();
        for line in BufReader::new(stdout).lines() {
            let line = line.map_err(|error| format!("fixture stdout read failed: {error}"))?;
            let _ = sender.send(line.clone());
            captured.push(line);
        }
        Ok(captured)
    });
    (receiver, worker)
}

fn read_stderr(stderr: ChildStderr) -> JoinHandle<Result<String, String>> {
    thread::spawn(move || {
        let mut captured = String::new();
        BufReader::new(stderr)
            .read_to_string(&mut captured)
            .map_err(|error| format!("fixture stderr read failed: {error}"))?;
        Ok(captured)
    })
}

fn process_start_time_100ns(pid: u32) -> u64 {
    let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) }
        .expect("could not open C++ Core fixture for identity verification");
    let mut creation = FILETIME::default();
    let mut exit = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    let result =
        unsafe { GetProcessTimes(handle, &mut creation, &mut exit, &mut kernel, &mut user) };
    let _ = unsafe { CloseHandle(handle) };
    result.expect("could not read C++ Core fixture creation time");
    ((creation.dwHighDateTime as u64) << 32) | creation.dwLowDateTime as u64
}

#[test]
fn session_manager_consumes_real_cpp_core_snapshot_event_and_shutdown() {
    let executable = fixture_executable();
    let mut fixture = FixtureProcess::spawn(&executable);
    let pid = fixture.child.id();
    let stdout = fixture
        .child
        .stdout
        .take()
        .expect("C++ Core fixture stdout was not piped");
    let stderr = fixture
        .child
        .stderr
        .take()
        .expect("C++ Core fixture stderr was not piped");
    let (stdout_lines, stdout_worker) = read_stdout(stdout);
    let stderr_worker = read_stderr(stderr);

    let ready_line = stdout_lines
        .recv_timeout(Duration::from_secs(10))
        .expect("C++ Core fixture did not publish its bounded readiness record");
    let ready: Value = serde_json::from_str(
        ready_line
            .strip_prefix(READY_PREFIX)
            .expect("C++ Core fixture emitted an unexpected readiness prefix"),
    )
    .expect("C++ Core fixture readiness record is not JSON");
    assert_eq!(ready["pid"], pid);
    assert_eq!(ready["session_id"], "fixture-host-session");
    assert_eq!(ready["snapshot_generation"], 1);
    assert_eq!(ready["snapshot_records"], 3);

    let target = TargetProcessIdentity::new(
        pid,
        process_start_time_100ns(pid),
        executable.to_string_lossy().into_owned(),
    )
    .expect("C++ Core fixture identity was rejected");
    let manager = SessionManager::new();
    let session = manager
        .connect(
            target,
            "rust-host-cross-language-fixture-0.1.0",
            Duration::from_secs(10),
        )
        .expect("SessionManager did not connect to the real C++ Core server");

    assert_eq!(session.target_pid(), pid);
    assert_eq!(session.session_id(), "fixture-host-session");
    assert_eq!(session.welcome().target_pid, pid);
    assert_eq!(
        session.welcome().capabilities.get("engine.core"),
        Some(&true)
    );
    assert_eq!(
        session.welcome().capabilities.get("transport.named_pipe"),
        Some(&true)
    );
    assert_eq!(
        session.welcome().capabilities.get("objects.snapshot"),
        Some(&true)
    );

    let subscription = session
        .subscribe_events(EventFilter::default(), Some(0), 8)
        .expect("real C++ Core event subscription failed");
    let delivered = subscription
        .recv_timeout(Duration::from_secs(5))
        .expect("real C++ Core event did not reach SessionManager EventHub");
    assert_eq!(delivered.event.seq, 1);
    assert_eq!(delivered.event.kind, "fixture.ready");
    assert_eq!(delivered.event.session_id, "fixture-host-session");
    assert_eq!(delivered.event.dropped_before, 0);
    assert_eq!(delivered.host_dropped_before, 0);
    assert_eq!(delivered.event.data["source"], "cpp-core");
    assert_eq!(delivered.event.data["snapshot_generation"], 1);

    let snapshot = session
        .refresh_snapshot(Duration::from_secs(5))
        .expect("SessionManager did not assemble the real C++ Core snapshot");
    assert_eq!(snapshot.session_id(), "fixture-host-session");
    assert_eq!(snapshot.generation(), 1);
    assert_eq!(snapshot.context_generation(), 901);
    assert_eq!(snapshot.record_count(), 3);
    assert_eq!(snapshot.records()[0].handle.index, 1);
    assert_eq!(snapshot.records()[2].handle.index, 7);

    let query = SnapshotQuery {
        kind: Some(SnapshotObjectKind::Class),
        search: Some("object1".to_string()),
        ..SnapshotQuery::default()
    };
    let page = session
        .query_snapshot(&query, None, 8)
        .expect("Host snapshot index rejected a real C++ Core record");
    assert_eq!(page.matched_count, 1);
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].kind, SnapshotObjectKind::Class);
    assert_eq!(page.items[0].full_path, "/Script/Fixture.Object1");

    let diagnostics = session.diagnostics().expect("session diagnostics failed");
    assert_eq!(diagnostics.phase, SessionPhase::Ready);
    assert_eq!(diagnostics.target_pid, pid);
    assert_eq!(diagnostics.server_pid, pid);
    assert_eq!(diagnostics.core_session_id, "fixture-host-session");
    assert_eq!(diagnostics.snapshot_generation, Some(1));
    assert_eq!(diagnostics.snapshot_context_generation, Some(901));
    assert_eq!(diagnostics.snapshot_record_count, Some(3));

    drop(subscription);
    manager
        .disconnect(
            pid,
            "cross_language_fixture_complete",
            Duration::from_secs(5),
        )
        .expect("SessionManager did not complete exact Shutdown against C++ Core");
    assert!(manager
        .active()
        .expect("active session query failed")
        .is_none());

    let status = fixture.finish(Duration::from_secs(10));
    let stdout = stdout_worker
        .join()
        .expect("fixture stdout reader panicked")
        .expect("fixture stdout reader failed");
    let stderr = stderr_worker
        .join()
        .expect("fixture stderr reader panicked")
        .expect("fixture stderr reader failed");
    assert!(
        status.success(),
        "C++ Core fixture exited with {status}; stdout={stdout:?}; stderr={stderr}"
    );
}
