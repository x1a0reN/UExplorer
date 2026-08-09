use crate::session::event_hub::{EventHubError, EventSubscription, HostEvent};
use serde::Serialize;
use std::collections::BTreeMap;
use std::fmt;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::Duration;
use uexplorer_protocol::MAX_GENERATION;

pub const MAX_EVENT_BRIDGES: usize = 64;

const BRIDGE_RECEIVE_WAIT: Duration = Duration::from_millis(50);

pub trait EventSink: Send + 'static {
    fn send(&self, event: HostEvent) -> Result<(), String>;
}

impl<F> EventSink for F
where
    F: Fn(HostEvent) -> Result<(), String> + Send + 'static,
{
    fn send(&self, event: HostEvent) -> Result<(), String> {
        self(event)
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct EventBridgeFailure {
    pub code: String,
    pub message: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct EventBridgeDiagnostics {
    pub bridge_id: u64,
    pub target_pid: u32,
    pub source_subscription_id: u64,
    pub delivered_events: u64,
    pub stopping: bool,
    pub finished: bool,
    pub failure: Option<EventBridgeFailure>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum EventBridgeError {
    InvalidTargetPid,
    LimitReached,
    IdExhausted,
    NotFound(u64),
    WorkerCreate(String),
    WorkerPanicked,
    LockPoisoned(&'static str),
}

impl EventBridgeError {
    pub fn code(&self) -> &'static str {
        match self {
            Self::InvalidTargetPid => "EVENT_BRIDGE_PID_INVALID",
            Self::LimitReached => "EVENT_BRIDGE_LIMIT_REACHED",
            Self::IdExhausted => "EVENT_BRIDGE_ID_EXHAUSTED",
            Self::NotFound(_) => "EVENT_BRIDGE_NOT_FOUND",
            Self::WorkerCreate(_) => "EVENT_BRIDGE_WORKER_CREATE_FAILED",
            Self::WorkerPanicked => "EVENT_BRIDGE_WORKER_PANICKED",
            Self::LockPoisoned(_) => "EVENT_BRIDGE_LOCK_POISONED",
        }
    }
}

impl fmt::Display for EventBridgeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidTargetPid => write!(formatter, "event bridge target PID must be positive"),
            Self::LimitReached => write!(formatter, "event bridge limit reached"),
            Self::IdExhausted => write!(formatter, "event bridge IDs exhausted"),
            Self::NotFound(id) => write!(formatter, "event bridge {id} was not found"),
            Self::WorkerCreate(message) => {
                write!(formatter, "event bridge worker creation failed: {message}")
            }
            Self::WorkerPanicked => write!(formatter, "event bridge worker panicked"),
            Self::LockPoisoned(name) => write!(formatter, "{name} lock is poisoned"),
        }
    }
}

impl std::error::Error for EventBridgeError {}

pub struct EventBridgeManager {
    state: Mutex<EventBridgeState>,
}

struct EventBridgeState {
    next_id: u64,
    workers: BTreeMap<u64, EventBridgeWorker>,
}

struct EventBridgeWorker {
    target_pid: u32,
    source_subscription_id: u64,
    stop: Arc<AtomicBool>,
    delivered_events: Arc<AtomicU64>,
    failure: Arc<Mutex<Option<EventBridgeFailure>>>,
    join: JoinHandle<()>,
}

impl Default for EventBridgeManager {
    fn default() -> Self {
        Self::new()
    }
}

impl EventBridgeManager {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(EventBridgeState {
                next_id: 1,
                workers: BTreeMap::new(),
            }),
        }
    }

    pub fn subscribe(
        &self,
        target_pid: u32,
        subscription: EventSubscription,
        sink: Box<dyn EventSink>,
    ) -> Result<EventBridgeDiagnostics, EventBridgeError> {
        if target_pid == 0 {
            return Err(EventBridgeError::InvalidTargetPid);
        }
        let mut state = lock(&self.state, "event bridge manager")?;
        reap_finished(&mut state)?;
        if state.workers.len() >= MAX_EVENT_BRIDGES {
            return Err(EventBridgeError::LimitReached);
        }
        let bridge_id = state.next_id;
        if bridge_id == 0 || bridge_id > MAX_GENERATION {
            return Err(EventBridgeError::IdExhausted);
        }
        state.next_id = if bridge_id == MAX_GENERATION {
            0
        } else {
            bridge_id + 1
        };

        let source_subscription_id = subscription.id();
        let stop = Arc::new(AtomicBool::new(false));
        let delivered_events = Arc::new(AtomicU64::new(0));
        let failure = Arc::new(Mutex::new(None));
        let worker_stop = Arc::clone(&stop);
        let worker_delivered = Arc::clone(&delivered_events);
        let worker_failure = Arc::clone(&failure);
        let join = thread::Builder::new()
            .name(format!("uexplorer-tauri-event-{target_pid}-{bridge_id}"))
            .spawn(move || {
                run_bridge(
                    subscription,
                    sink,
                    worker_stop,
                    worker_delivered,
                    worker_failure,
                )
            })
            .map_err(|error| EventBridgeError::WorkerCreate(error.to_string()))?;

        state.workers.insert(
            bridge_id,
            EventBridgeWorker {
                target_pid,
                source_subscription_id,
                stop,
                delivered_events,
                failure,
                join,
            },
        );
        diagnostics_for(
            bridge_id,
            state
                .workers
                .get(&bridge_id)
                .expect("new event bridge must be registered"),
        )
    }

    pub fn unsubscribe(&self, bridge_id: u64) -> Result<(), EventBridgeError> {
        let worker = lock(&self.state, "event bridge manager")?
            .workers
            .remove(&bridge_id)
            .ok_or(EventBridgeError::NotFound(bridge_id))?;
        stop_and_join(worker)
    }

    pub fn stop_session(&self, target_pid: u32) -> Result<usize, EventBridgeError> {
        if target_pid == 0 {
            return Err(EventBridgeError::InvalidTargetPid);
        }
        let workers = {
            let mut state = lock(&self.state, "event bridge manager")?;
            let ids = state
                .workers
                .iter()
                .filter_map(|(id, worker)| (worker.target_pid == target_pid).then_some(*id))
                .collect::<Vec<_>>();
            ids.into_iter()
                .filter_map(|id| state.workers.remove(&id))
                .collect::<Vec<_>>()
        };
        let count = workers.len();
        stop_and_join_all(workers)?;
        Ok(count)
    }

    pub fn diagnostics(&self) -> Result<Vec<EventBridgeDiagnostics>, EventBridgeError> {
        let state = lock(&self.state, "event bridge manager")?;
        state
            .workers
            .iter()
            .map(|(id, worker)| diagnostics_for(*id, worker))
            .collect()
    }
}

impl Drop for EventBridgeManager {
    fn drop(&mut self) {
        let workers = match self.state.get_mut() {
            Ok(state) => std::mem::take(&mut state.workers),
            Err(poisoned) => std::mem::take(&mut poisoned.into_inner().workers),
        };
        let _ = stop_and_join_all(workers.into_values().collect());
    }
}

fn run_bridge(
    subscription: EventSubscription,
    sink: Box<dyn EventSink>,
    stop: Arc<AtomicBool>,
    delivered_events: Arc<AtomicU64>,
    failure: Arc<Mutex<Option<EventBridgeFailure>>>,
) {
    while !stop.load(Ordering::Acquire) {
        match subscription.recv_timeout(BRIDGE_RECEIVE_WAIT) {
            Ok(event) => match sink.send(event) {
                Ok(()) => increment(&delivered_events),
                Err(message) => {
                    record_failure(&failure, "TAURI_EVENT_DELIVERY_FAILED", message);
                    break;
                }
            },
            Err(EventHubError::ReceiveTimeout) => {}
            Err(EventHubError::SubscriptionDisconnected) => break,
            Err(error) => {
                record_failure(&failure, error.code(), error.to_string());
                break;
            }
        }
    }
}

fn increment(counter: &AtomicU64) {
    let _ = counter.fetch_update(Ordering::AcqRel, Ordering::Acquire, |value| {
        Some(value.saturating_add(1).min(MAX_GENERATION))
    });
}

fn record_failure(
    failure: &Mutex<Option<EventBridgeFailure>>,
    code: impl Into<String>,
    message: impl Into<String>,
) {
    if let Ok(mut slot) = failure.lock() {
        *slot = Some(EventBridgeFailure {
            code: code.into(),
            message: message.into(),
        });
    }
}

fn diagnostics_for(
    bridge_id: u64,
    worker: &EventBridgeWorker,
) -> Result<EventBridgeDiagnostics, EventBridgeError> {
    Ok(EventBridgeDiagnostics {
        bridge_id,
        target_pid: worker.target_pid,
        source_subscription_id: worker.source_subscription_id,
        delivered_events: worker.delivered_events.load(Ordering::Acquire),
        stopping: worker.stop.load(Ordering::Acquire),
        finished: worker.join.is_finished(),
        failure: lock(&worker.failure, "event bridge failure")?.clone(),
    })
}

fn reap_finished(state: &mut EventBridgeState) -> Result<(), EventBridgeError> {
    let finished = state
        .workers
        .iter()
        .filter_map(|(id, worker)| worker.join.is_finished().then_some(*id))
        .collect::<Vec<_>>();
    for id in finished {
        if let Some(worker) = state.workers.remove(&id) {
            worker
                .join
                .join()
                .map_err(|_| EventBridgeError::WorkerPanicked)?;
        }
    }
    Ok(())
}

fn stop_and_join(worker: EventBridgeWorker) -> Result<(), EventBridgeError> {
    stop_and_join_all(vec![worker])
}

fn stop_and_join_all(workers: Vec<EventBridgeWorker>) -> Result<(), EventBridgeError> {
    for worker in &workers {
        worker.stop.store(true, Ordering::Release);
    }
    let mut worker_panicked = false;
    for worker in workers {
        if worker.join.join().is_err() {
            worker_panicked = true;
        }
    }
    if worker_panicked {
        Err(EventBridgeError::WorkerPanicked)
    } else {
        Ok(())
    }
}

fn lock<'a, T>(
    mutex: &'a Mutex<T>,
    name: &'static str,
) -> Result<MutexGuard<'a, T>, EventBridgeError> {
    mutex
        .lock()
        .map_err(|_| EventBridgeError::LockPoisoned(name))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::event_hub::{EventFilter, EventHub};
    use serde_json::json;
    use std::sync::mpsc;
    use uexplorer_protocol::EventPayload;

    fn event(session_id: &str, seq: u64) -> EventPayload {
        EventPayload {
            seq,
            kind: "runtime.tick".to_string(),
            timestamp_us: seq,
            session_id: session_id.to_string(),
            dropped_before: 0,
            data: json!({}),
        }
    }

    #[test]
    fn bridge_is_owned_delivers_and_unsubscribes_exactly() {
        let hub = EventHub::new("session-1").unwrap();
        let subscription = hub.subscribe(EventFilter::default(), None, 4).unwrap();
        let (tx, rx) = mpsc::sync_channel(4);
        let manager = EventBridgeManager::new();
        let diagnostics = manager
            .subscribe(
                101,
                subscription,
                Box::new(move |event| tx.send(event).map_err(|error| error.to_string())),
            )
            .unwrap();
        hub.publish(event("session-1", 1), 0).unwrap();
        assert_eq!(
            rx.recv_timeout(Duration::from_secs(1)).unwrap().event.seq,
            1
        );

        manager.unsubscribe(diagnostics.bridge_id).unwrap();
        assert!(manager.diagnostics().unwrap().is_empty());
        assert_eq!(hub.diagnostics().unwrap().subscriber_count, 0);
    }

    #[test]
    fn failed_sink_is_diagnostic_and_reaped_before_new_admission() {
        let hub = EventHub::new("session-2").unwrap();
        let subscription = hub.subscribe(EventFilter::default(), None, 1).unwrap();
        let manager = EventBridgeManager::new();
        let first = manager
            .subscribe(
                202,
                subscription,
                Box::new(|_| Err("fixture delivery failure".to_string())),
            )
            .unwrap();
        hub.publish(event("session-2", 1), 0).unwrap();

        let deadline = std::time::Instant::now() + Duration::from_secs(1);
        loop {
            let diagnostics = manager.diagnostics().unwrap();
            if diagnostics[0].finished {
                assert_eq!(
                    diagnostics[0].failure.as_ref().unwrap().code,
                    "TAURI_EVENT_DELIVERY_FAILED"
                );
                break;
            }
            assert!(std::time::Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }

        let replacement = hub.subscribe(EventFilter::default(), None, 1).unwrap();
        let second = manager
            .subscribe(202, replacement, Box::new(|_| Ok(())))
            .unwrap();
        assert!(second.bridge_id > first.bridge_id);
        assert_eq!(manager.diagnostics().unwrap().len(), 1);
        manager.unsubscribe(second.bridge_id).unwrap();
    }

    #[test]
    fn stopping_one_pid_does_not_stop_another_pid_bridge() {
        let first_hub = EventHub::new("session-3").unwrap();
        let second_hub = EventHub::new("session-4").unwrap();
        let manager = EventBridgeManager::new();
        manager
            .subscribe(
                303,
                first_hub
                    .subscribe(EventFilter::default(), None, 1)
                    .unwrap(),
                Box::new(|_| Ok(())),
            )
            .unwrap();
        let second = manager
            .subscribe(
                404,
                second_hub
                    .subscribe(EventFilter::default(), None, 1)
                    .unwrap(),
                Box::new(|_| Ok(())),
            )
            .unwrap();

        assert_eq!(manager.stop_session(303).unwrap(), 1);
        assert_eq!(manager.diagnostics().unwrap().len(), 1);
        assert_eq!(manager.diagnostics().unwrap()[0].target_pid, 404);
        manager.unsubscribe(second.bridge_id).unwrap();
    }

    #[test]
    fn final_javascript_safe_bridge_id_is_usable_before_exhaustion() {
        let hub = EventHub::new("session-5").unwrap();
        let manager = EventBridgeManager::new();
        manager.state.lock().unwrap().next_id = MAX_GENERATION;

        let final_bridge = manager
            .subscribe(
                505,
                hub.subscribe(EventFilter::default(), None, 1).unwrap(),
                Box::new(|_| Ok(())),
            )
            .unwrap();
        assert_eq!(final_bridge.bridge_id, MAX_GENERATION);
        manager.unsubscribe(final_bridge.bridge_id).unwrap();

        let error = manager
            .subscribe(
                505,
                hub.subscribe(EventFilter::default(), None, 1).unwrap(),
                Box::new(|_| Ok(())),
            )
            .unwrap_err();
        assert_eq!(error, EventBridgeError::IdExhausted);
    }

    #[test]
    fn panicked_bridge_does_not_detach_sibling_workers_during_session_stop() {
        let hub = EventHub::new("session-6").unwrap();
        let manager = EventBridgeManager::new();
        manager
            .subscribe(
                606,
                hub.subscribe(EventFilter::default(), None, 1).unwrap(),
                Box::new(|_| panic!("fixture bridge panic")),
            )
            .unwrap();
        manager
            .subscribe(
                606,
                hub.subscribe(EventFilter::default(), None, 1).unwrap(),
                Box::new(|_| Ok(())),
            )
            .unwrap();
        hub.publish(event("session-6", 1), 0).unwrap();

        let deadline = std::time::Instant::now() + Duration::from_secs(1);
        while !manager.diagnostics().unwrap()[0].finished {
            assert!(std::time::Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }

        assert_eq!(
            manager.stop_session(606),
            Err(EventBridgeError::WorkerPanicked)
        );
        assert!(manager.diagnostics().unwrap().is_empty());
        assert_eq!(hub.diagnostics().unwrap().subscriber_count, 0);
    }
}
