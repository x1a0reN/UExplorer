use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fmt;
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender, TryRecvError, TrySendError};
use std::sync::{Arc, Mutex, MutexGuard, Weak};
use std::time::Duration;
use uexplorer_protocol::{EventPayload, MAX_GENERATION};

pub const MAX_EVENT_SUBSCRIBERS: usize = 64;
pub const MAX_SUBSCRIBER_EVENTS: usize = 1_024;
pub const MAX_REPLAY_EVENTS: usize = 1_024;
pub const MAX_REPLAY_EVENT_BYTES: usize = 64 * 1_024;

const MAX_EVENT_KIND_BYTES: usize = 128;
const MAX_FILTER_VALUES: usize = 64;
const MAX_SELECTOR_BYTES: usize = 256;
const MAX_SESSION_ID_BYTES: usize = 128;

#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct EventFilter {
    #[serde(default)]
    pub kinds: BTreeSet<String>,
    #[serde(default)]
    pub watch_ids: BTreeSet<String>,
    #[serde(default)]
    pub hook_names: BTreeSet<String>,
}

impl EventFilter {
    fn validate(&self) -> Result<(), EventHubError> {
        if self.kinds.len() > MAX_FILTER_VALUES
            || self.watch_ids.len() > MAX_FILTER_VALUES
            || self.hook_names.len() > MAX_FILTER_VALUES
        {
            return Err(EventHubError::InvalidFilter(
                "each filter selector is limited to 64 values",
            ));
        }
        if self.kinds.iter().any(|kind| !is_valid_event_kind(kind)) {
            return Err(EventHubError::InvalidFilter(
                "event kinds must use the bounded v1 lowercase grammar",
            ));
        }
        if self
            .watch_ids
            .iter()
            .chain(self.hook_names.iter())
            .any(|value| !is_bounded_text(value, MAX_SELECTOR_BYTES))
        {
            return Err(EventHubError::InvalidFilter(
                "watch and hook selectors must be bounded text",
            ));
        }
        Ok(())
    }

    fn matches(&self, event: &EventPayload) -> bool {
        (self.kinds.is_empty() || self.kinds.contains(&event.kind))
            && selector_matches(&self.watch_ids, &event.data, "watch_id")
            && selector_matches(&self.hook_names, &event.data, "hook_name")
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct HostEvent {
    pub event: Arc<EventPayload>,
    pub host_dropped_before: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct EventHubDiagnostics {
    pub session_id: String,
    pub closed: bool,
    pub subscriber_count: usize,
    pub replay_event_count: usize,
    pub replay_oldest_seq: Option<u64>,
    pub last_seq: u64,
    pub core_dropped_before: u64,
    pub transport_dropped_before: u64,
    pub published_events: u64,
    pub delivered_events: u64,
    pub subscriber_delivery_drops: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum EventHubError {
    InvalidSession,
    InvalidEvent(&'static str),
    InvalidFilter(&'static str),
    InvalidCapacity,
    SubscriberLimitReached,
    SubscriptionIdExhausted,
    SequenceRegression {
        previous: u64,
        actual: u64,
    },
    CoreDropRegression {
        previous: u64,
        actual: u64,
    },
    TransportDropRegression {
        previous: u64,
        actual: u64,
    },
    ReplayCursorAhead {
        requested: u64,
        latest: u64,
    },
    ReplayUnavailable {
        requested_after: u64,
        earliest_available: u64,
    },
    ReplayExceedsCapacity {
        required: usize,
        capacity: usize,
    },
    Closed,
    ReceiveTimeout,
    SubscriptionDisconnected,
    LockPoisoned,
}

impl EventHubError {
    pub fn code(&self) -> &'static str {
        match self {
            Self::InvalidSession => "EVENT_SESSION_INVALID",
            Self::InvalidEvent(_) => "EVENT_ENVELOPE_INVALID",
            Self::InvalidFilter(_) => "EVENT_FILTER_INVALID",
            Self::InvalidCapacity => "EVENT_SUBSCRIBER_CAPACITY_INVALID",
            Self::SubscriberLimitReached => "EVENT_SUBSCRIBER_LIMIT_REACHED",
            Self::SubscriptionIdExhausted => "EVENT_SUBSCRIPTION_ID_EXHAUSTED",
            Self::SequenceRegression { .. } => "EVENT_SEQUENCE_REGRESSION",
            Self::CoreDropRegression { .. } => "EVENT_CORE_DROP_REGRESSION",
            Self::TransportDropRegression { .. } => "EVENT_TRANSPORT_DROP_REGRESSION",
            Self::ReplayCursorAhead { .. } => "EVENT_REPLAY_CURSOR_AHEAD",
            Self::ReplayUnavailable { .. } => "EVENT_REPLAY_UNAVAILABLE",
            Self::ReplayExceedsCapacity { .. } => "EVENT_REPLAY_CAPACITY_EXCEEDED",
            Self::Closed => "EVENT_HUB_CLOSED",
            Self::ReceiveTimeout => "EVENT_RECEIVE_TIMEOUT",
            Self::SubscriptionDisconnected => "EVENT_SUBSCRIPTION_DISCONNECTED",
            Self::LockPoisoned => "EVENT_HUB_LOCK_POISONED",
        }
    }
}

impl fmt::Display for EventHubError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidSession => write!(formatter, "event session ID is invalid"),
            Self::InvalidEvent(reason) => write!(formatter, "invalid event envelope: {reason}"),
            Self::InvalidFilter(reason) => write!(formatter, "invalid event filter: {reason}"),
            Self::InvalidCapacity => write!(formatter, "subscriber capacity must be 1..1024"),
            Self::SubscriberLimitReached => write!(formatter, "event subscriber limit reached"),
            Self::SubscriptionIdExhausted => write!(formatter, "event subscription IDs exhausted"),
            Self::SequenceRegression { previous, actual } => write!(
                formatter,
                "event sequence did not increase: previous {previous}, actual {actual}"
            ),
            Self::CoreDropRegression { previous, actual } => write!(
                formatter,
                "Core drop count regressed: previous {previous}, actual {actual}"
            ),
            Self::TransportDropRegression { previous, actual } => write!(
                formatter,
                "transport drop count regressed: previous {previous}, actual {actual}"
            ),
            Self::ReplayCursorAhead { requested, latest } => write!(
                formatter,
                "replay cursor {requested} is ahead of latest event {latest}"
            ),
            Self::ReplayUnavailable {
                requested_after,
                earliest_available,
            } => write!(
                formatter,
                "events after {requested_after} are no longer retained; earliest available is {earliest_available}"
            ),
            Self::ReplayExceedsCapacity { required, capacity } => write!(
                formatter,
                "replay requires {required} queue slots but subscriber capacity is {capacity}"
            ),
            Self::Closed => write!(formatter, "event hub is closed"),
            Self::ReceiveTimeout => write!(formatter, "event receive timed out"),
            Self::SubscriptionDisconnected => write!(formatter, "event subscription disconnected"),
            Self::LockPoisoned => write!(formatter, "event hub lock is poisoned"),
        }
    }
}

impl std::error::Error for EventHubError {}

#[derive(Clone)]
pub struct EventHub {
    inner: Arc<EventHubInner>,
}

struct EventHubInner {
    session_id: String,
    state: Mutex<EventHubState>,
}

struct EventHubState {
    closed: bool,
    next_subscription_id: u64,
    last_seq: u64,
    core_dropped_before: u64,
    transport_dropped_before: u64,
    published_events: u64,
    delivered_events: u64,
    subscriber_delivery_drops: u64,
    history: VecDeque<RetainedEvent>,
    subscribers: BTreeMap<u64, SubscriberState>,
}

#[derive(Clone)]
struct RetainedEvent {
    event: Arc<EventPayload>,
    transport_dropped_before: u64,
}

struct SubscriberState {
    filter: EventFilter,
    sender: SyncSender<HostEvent>,
    dropped_before: u64,
}

pub struct EventSubscription {
    id: u64,
    receiver: Receiver<HostEvent>,
    hub: Weak<EventHubInner>,
}

impl EventHub {
    pub fn new(session_id: impl Into<String>) -> Result<Self, EventHubError> {
        let session_id = session_id.into();
        if !is_valid_session_id(&session_id) {
            return Err(EventHubError::InvalidSession);
        }
        Ok(Self {
            inner: Arc::new(EventHubInner {
                session_id,
                state: Mutex::new(EventHubState {
                    closed: false,
                    next_subscription_id: 1,
                    last_seq: 0,
                    core_dropped_before: 0,
                    transport_dropped_before: 0,
                    published_events: 0,
                    delivered_events: 0,
                    subscriber_delivery_drops: 0,
                    history: VecDeque::with_capacity(MAX_REPLAY_EVENTS),
                    subscribers: BTreeMap::new(),
                }),
            }),
        })
    }

    pub fn session_id(&self) -> &str {
        &self.inner.session_id
    }

    pub fn publish(
        &self,
        event: EventPayload,
        transport_dropped_before: u64,
    ) -> Result<(), EventHubError> {
        validate_event(&self.inner.session_id, &event)?;
        if transport_dropped_before > MAX_GENERATION {
            return Err(EventHubError::InvalidEvent(
                "transport drop count exceeds the protocol integer range",
            ));
        }
        let encoded_size = serde_json::to_vec(&event)
            .map_err(|_| EventHubError::InvalidEvent("event serialization failed"))?
            .len();
        if encoded_size > MAX_REPLAY_EVENT_BYTES {
            return Err(EventHubError::InvalidEvent(
                "serialized event exceeds the 64 KiB Host event limit",
            ));
        }
        let event = Arc::new(event);
        let mut state = lock(&self.inner.state)?;
        if state.closed {
            return Err(EventHubError::Closed);
        }
        if event.seq <= state.last_seq {
            return Err(EventHubError::SequenceRegression {
                previous: state.last_seq,
                actual: event.seq,
            });
        }
        if event.dropped_before < state.core_dropped_before {
            return Err(EventHubError::CoreDropRegression {
                previous: state.core_dropped_before,
                actual: event.dropped_before,
            });
        }
        if transport_dropped_before < state.transport_dropped_before {
            return Err(EventHubError::TransportDropRegression {
                previous: state.transport_dropped_before,
                actual: transport_dropped_before,
            });
        }
        if state.last_seq != 0 && event.seq > state.last_seq + 1 {
            let missing = event.seq - state.last_seq - 1;
            let core_drops = event.dropped_before - state.core_dropped_before;
            let transport_drops = transport_dropped_before - state.transport_dropped_before;
            if core_drops.saturating_add(transport_drops) < missing {
                return Err(EventHubError::InvalidEvent(
                    "a sequence gap is not accounted for by Core or transport drops",
                ));
            }
        }

        state.last_seq = event.seq;
        state.core_dropped_before = event.dropped_before;
        state.transport_dropped_before = transport_dropped_before;
        state.published_events = increment_counter(state.published_events, 1);
        if state.history.len() == MAX_REPLAY_EVENTS {
            state.history.pop_front();
        }
        state.history.push_back(RetainedEvent {
            event: Arc::clone(&event),
            transport_dropped_before,
        });

        let mut disconnected = Vec::new();
        let mut delivered = 0u64;
        let mut dropped = 0u64;
        for (id, subscriber) in &mut state.subscribers {
            if !subscriber.filter.matches(&event) {
                continue;
            }
            let envelope = HostEvent {
                event: Arc::clone(&event),
                host_dropped_before: increment_counter(
                    transport_dropped_before,
                    subscriber.dropped_before,
                ),
            };
            match subscriber.sender.try_send(envelope) {
                Ok(()) => delivered = increment_counter(delivered, 1),
                Err(TrySendError::Full(_)) => {
                    subscriber.dropped_before = increment_counter(subscriber.dropped_before, 1);
                    dropped = increment_counter(dropped, 1);
                }
                Err(TrySendError::Disconnected(_)) => disconnected.push(*id),
            }
        }
        for id in disconnected {
            state.subscribers.remove(&id);
        }
        state.delivered_events = increment_counter(state.delivered_events, delivered);
        state.subscriber_delivery_drops =
            increment_counter(state.subscriber_delivery_drops, dropped);
        Ok(())
    }

    pub fn subscribe(
        &self,
        filter: EventFilter,
        replay_after_seq: Option<u64>,
        capacity: usize,
    ) -> Result<EventSubscription, EventHubError> {
        filter.validate()?;
        if capacity == 0 || capacity > MAX_SUBSCRIBER_EVENTS {
            return Err(EventHubError::InvalidCapacity);
        }
        if replay_after_seq.is_some_and(|seq| seq > MAX_GENERATION) {
            return Err(EventHubError::InvalidFilter(
                "replay cursor exceeds the protocol integer range",
            ));
        }

        let mut state = lock(&self.inner.state)?;
        if state.closed {
            return Err(EventHubError::Closed);
        }
        if state.subscribers.len() >= MAX_EVENT_SUBSCRIBERS {
            return Err(EventHubError::SubscriberLimitReached);
        }
        let replay = replay_events(&state, &filter, replay_after_seq)?;
        if replay.len() > capacity {
            return Err(EventHubError::ReplayExceedsCapacity {
                required: replay.len(),
                capacity,
            });
        }
        let subscription_id = state.next_subscription_id;
        state.next_subscription_id = state
            .next_subscription_id
            .checked_add(1)
            .ok_or(EventHubError::SubscriptionIdExhausted)?;

        let (sender, receiver) = mpsc::sync_channel(capacity);
        for retained in replay {
            sender
                .try_send(HostEvent {
                    event: retained.event,
                    host_dropped_before: retained.transport_dropped_before,
                })
                .map_err(|_| EventHubError::ReplayExceedsCapacity {
                    required: capacity.saturating_add(1),
                    capacity,
                })?;
            state.delivered_events = increment_counter(state.delivered_events, 1);
        }
        state.subscribers.insert(
            subscription_id,
            SubscriberState {
                filter,
                sender,
                dropped_before: 0,
            },
        );
        Ok(EventSubscription {
            id: subscription_id,
            receiver,
            hub: Arc::downgrade(&self.inner),
        })
    }

    pub fn diagnostics(&self) -> Result<EventHubDiagnostics, EventHubError> {
        let state = lock(&self.inner.state)?;
        Ok(EventHubDiagnostics {
            session_id: self.inner.session_id.clone(),
            closed: state.closed,
            subscriber_count: state.subscribers.len(),
            replay_event_count: state.history.len(),
            replay_oldest_seq: state.history.front().map(|retained| retained.event.seq),
            last_seq: state.last_seq,
            core_dropped_before: state.core_dropped_before,
            transport_dropped_before: state.transport_dropped_before,
            published_events: state.published_events,
            delivered_events: state.delivered_events,
            subscriber_delivery_drops: state.subscriber_delivery_drops,
        })
    }

    pub fn close(&self) -> Result<(), EventHubError> {
        let mut state = lock(&self.inner.state)?;
        state.closed = true;
        state.subscribers.clear();
        Ok(())
    }
}

impl EventSubscription {
    pub fn id(&self) -> u64 {
        self.id
    }

    pub fn try_recv(&self) -> Result<Option<HostEvent>, EventHubError> {
        match self.receiver.try_recv() {
            Ok(event) => Ok(Some(event)),
            Err(TryRecvError::Empty) => Ok(None),
            Err(TryRecvError::Disconnected) => Err(EventHubError::SubscriptionDisconnected),
        }
    }

    pub fn recv_timeout(&self, timeout: Duration) -> Result<HostEvent, EventHubError> {
        match self.receiver.recv_timeout(timeout) {
            Ok(event) => Ok(event),
            Err(RecvTimeoutError::Timeout) => Err(EventHubError::ReceiveTimeout),
            Err(RecvTimeoutError::Disconnected) => Err(EventHubError::SubscriptionDisconnected),
        }
    }
}

impl Drop for EventSubscription {
    fn drop(&mut self) {
        let Some(hub) = self.hub.upgrade() else {
            return;
        };
        if let Ok(mut state) = hub.state.lock() {
            state.subscribers.remove(&self.id);
        };
    }
}

fn replay_events(
    state: &EventHubState,
    filter: &EventFilter,
    replay_after_seq: Option<u64>,
) -> Result<Vec<RetainedEvent>, EventHubError> {
    let Some(after_seq) = replay_after_seq else {
        return Ok(Vec::new());
    };
    if after_seq > state.last_seq {
        return Err(EventHubError::ReplayCursorAhead {
            requested: after_seq,
            latest: state.last_seq,
        });
    }
    if let Some(earliest) = state.history.front().map(|retained| retained.event.seq) {
        if after_seq.saturating_add(1) < earliest {
            return Err(EventHubError::ReplayUnavailable {
                requested_after: after_seq,
                earliest_available: earliest,
            });
        }
    }
    Ok(state
        .history
        .iter()
        .filter(|retained| retained.event.seq > after_seq && filter.matches(&retained.event))
        .cloned()
        .collect())
}

fn increment_counter(value: u64, increment: u64) -> u64 {
    value.saturating_add(increment).min(MAX_GENERATION)
}

fn validate_event(session_id: &str, event: &EventPayload) -> Result<(), EventHubError> {
    if event.session_id != session_id {
        return Err(EventHubError::InvalidSession);
    }
    if event.seq == 0 || event.seq > MAX_GENERATION {
        return Err(EventHubError::InvalidEvent(
            "sequence must be a positive protocol integer",
        ));
    }
    if event.timestamp_us > MAX_GENERATION {
        return Err(EventHubError::InvalidEvent(
            "timestamp exceeds the protocol integer range",
        ));
    }
    if event.dropped_before > MAX_GENERATION || event.dropped_before >= event.seq {
        return Err(EventHubError::InvalidEvent(
            "Core drop count must describe events before this sequence",
        ));
    }
    if !is_valid_event_kind(&event.kind) {
        return Err(EventHubError::InvalidEvent(
            "kind does not match the bounded v1 grammar",
        ));
    }
    Ok(())
}

fn selector_matches(selectors: &BTreeSet<String>, data: &serde_json::Value, key: &str) -> bool {
    selectors.is_empty()
        || data
            .get(key)
            .and_then(serde_json::Value::as_str)
            .is_some_and(|value| selectors.contains(value))
}

fn is_valid_event_kind(value: &str) -> bool {
    if value.is_empty() || value.len() > MAX_EVENT_KIND_BYTES {
        return false;
    }
    value.bytes().enumerate().all(|(index, byte)| {
        if index == 0 {
            return byte.is_ascii_lowercase();
        }
        byte.is_ascii_lowercase()
            || byte.is_ascii_digit()
            || byte == b'_'
            || byte == b'-'
            || byte == b'.'
    })
}

fn is_valid_session_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_SESSION_ID_BYTES
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}

fn is_bounded_text(value: &str, maximum: usize) -> bool {
    !value.is_empty() && value.len() <= maximum && !value.chars().any(char::is_control)
}

fn lock<T>(mutex: &Mutex<T>) -> Result<MutexGuard<'_, T>, EventHubError> {
    mutex.lock().map_err(|_| EventHubError::LockPoisoned)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn event(seq: u64, kind: &str, dropped_before: u64, data: serde_json::Value) -> EventPayload {
        EventPayload {
            seq,
            kind: kind.to_string(),
            timestamp_us: seq,
            session_id: "session-1".to_string(),
            dropped_before,
            data,
        }
    }

    fn publish(hub: &EventHub, event: EventPayload) -> Result<(), EventHubError> {
        hub.publish(event, 0)
    }

    #[test]
    fn selectors_are_host_side_and_do_not_mutate_sequence() {
        let hub = EventHub::new("session-1").unwrap();
        let watch = hub
            .subscribe(
                EventFilter {
                    kinds: BTreeSet::from(["watch.changed".to_string()]),
                    watch_ids: BTreeSet::from(["watch-7".to_string()]),
                    hook_names: BTreeSet::new(),
                },
                None,
                4,
            )
            .unwrap();
        publish(
            &hub,
            event(1, "hook.called", 0, json!({"hook_name": "ProcessEvent"})),
        )
        .unwrap();
        publish(
            &hub,
            event(2, "watch.changed", 0, json!({"watch_id": "watch-8"})),
        )
        .unwrap();
        publish(
            &hub,
            event(3, "watch.changed", 0, json!({"watch_id": "watch-7"})),
        )
        .unwrap();

        assert_eq!(
            watch
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .event
                .seq,
            3
        );
        assert_eq!(watch.try_recv().unwrap(), None);
        assert_eq!(hub.diagnostics().unwrap().last_seq, 3);
    }

    #[test]
    fn slow_subscriber_has_a_monotonic_drop_counter_without_blocking_publish() {
        let hub = EventHub::new("session-1").unwrap();
        let subscriber = hub.subscribe(EventFilter::default(), None, 1).unwrap();
        publish(&hub, event(1, "runtime.ready", 0, json!({}))).unwrap();
        publish(&hub, event(2, "runtime.tick", 0, json!({}))).unwrap();

        let first = subscriber.recv_timeout(Duration::from_secs(1)).unwrap();
        assert_eq!(first.event.seq, 1);
        assert_eq!(first.host_dropped_before, 0);

        publish(&hub, event(3, "runtime.tick", 0, json!({}))).unwrap();
        let third = subscriber.recv_timeout(Duration::from_secs(1)).unwrap();
        assert_eq!(third.event.seq, 3);
        assert_eq!(third.host_dropped_before, 1);
        let diagnostics = hub.diagnostics().unwrap();
        assert_eq!(diagnostics.subscriber_delivery_drops, 1);
        assert_eq!(diagnostics.delivered_events, 2);
    }

    #[test]
    fn transport_drops_account_for_sequence_gaps_and_survive_replay() {
        let hub = EventHub::new("session-1").unwrap();
        let subscriber = hub.subscribe(EventFilter::default(), None, 4).unwrap();
        hub.publish(event(1, "runtime.ready", 0, json!({})), 0)
            .unwrap();
        hub.publish(event(3, "runtime.tick", 0, json!({})), 1)
            .unwrap();

        assert_eq!(
            subscriber
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .host_dropped_before,
            0
        );
        assert_eq!(
            subscriber
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .host_dropped_before,
            1
        );
        assert_eq!(hub.diagnostics().unwrap().transport_dropped_before, 1);

        let replay = hub.subscribe(EventFilter::default(), Some(0), 2).unwrap();
        assert_eq!(
            replay
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .host_dropped_before,
            0
        );
        assert_eq!(
            replay
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .host_dropped_before,
            1
        );
        assert!(matches!(
            hub.publish(event(4, "runtime.tick", 0, json!({})), 0),
            Err(EventHubError::TransportDropRegression { .. })
        ));
        assert!(matches!(
            hub.publish(event(5, "runtime.tick", 0, json!({})), 1),
            Err(EventHubError::InvalidEvent(_))
        ));
    }

    #[test]
    fn replay_is_exact_or_explicitly_rejected() {
        let hub = EventHub::new("session-1").unwrap();
        for seq in 1..=5 {
            publish(&hub, event(seq, "runtime.tick", 0, json!({}))).unwrap();
        }
        let replay = hub.subscribe(EventFilter::default(), Some(2), 3).unwrap();
        assert_eq!(
            replay
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .event
                .seq,
            3
        );
        assert_eq!(
            replay
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .event
                .seq,
            4
        );
        assert_eq!(
            replay
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .event
                .seq,
            5
        );
        assert!(matches!(
            hub.subscribe(EventFilter::default(), Some(2), 2),
            Err(EventHubError::ReplayExceedsCapacity {
                required: 3,
                capacity: 2
            })
        ));

        for seq in 6..=(MAX_REPLAY_EVENTS as u64 + 2) {
            publish(&hub, event(seq, "runtime.tick", 0, json!({}))).unwrap();
        }
        assert!(matches!(
            hub.subscribe(EventFilter::default(), Some(0), MAX_SUBSCRIBER_EVENTS),
            Err(EventHubError::ReplayUnavailable {
                requested_after: 0,
                earliest_available: 3
            })
        ));
    }

    #[test]
    fn invalid_session_sequence_drop_and_envelope_are_rejected() {
        let hub = EventHub::new("session-1").unwrap();
        let mut wrong_session = event(1, "runtime.ready", 0, json!({}));
        wrong_session.session_id = "session-2".to_string();
        assert_eq!(
            publish(&hub, wrong_session),
            Err(EventHubError::InvalidSession)
        );

        publish(&hub, event(2, "runtime.ready", 1, json!({}))).unwrap();
        assert!(matches!(
            publish(&hub, event(2, "runtime.ready", 1, json!({}))),
            Err(EventHubError::SequenceRegression { .. })
        ));
        assert!(matches!(
            publish(&hub, event(4, "runtime.ready", 1, json!({}))),
            Err(EventHubError::InvalidEvent(_))
        ));
        assert!(matches!(
            publish(&hub, event(3, "runtime.ready", 0, json!({}))),
            Err(EventHubError::CoreDropRegression { .. })
        ));
        assert!(matches!(
            publish(&hub, event(4, "Invalid Kind", 1, json!({}))),
            Err(EventHubError::InvalidEvent(_))
        ));

        let oversized = event(
            4,
            "runtime.ready",
            1,
            json!({"payload": "x".repeat(MAX_REPLAY_EVENT_BYTES)}),
        );
        assert!(matches!(
            publish(&hub, oversized),
            Err(EventHubError::InvalidEvent(
                "serialized event exceeds the 64 KiB Host event limit"
            ))
        ));
    }

    #[test]
    fn closing_or_dropping_subscription_releases_waiters_and_capacity() {
        let hub = EventHub::new("session-1").unwrap();
        let subscription = hub.subscribe(EventFilter::default(), None, 1).unwrap();
        assert_eq!(hub.diagnostics().unwrap().subscriber_count, 1);
        drop(subscription);
        assert_eq!(hub.diagnostics().unwrap().subscriber_count, 0);

        let subscription = hub.subscribe(EventFilter::default(), None, 1).unwrap();
        hub.close().unwrap();
        assert_eq!(
            subscription.recv_timeout(Duration::from_millis(1)),
            Err(EventHubError::SubscriptionDisconnected)
        );
        assert_eq!(
            publish(&hub, event(1, "runtime.ready", 0, json!({}))),
            Err(EventHubError::Closed)
        );
    }
}
