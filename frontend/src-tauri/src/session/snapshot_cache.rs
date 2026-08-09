use serde::{Deserialize, Serialize};
use std::borrow::Cow;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fmt;
use std::sync::{Arc, RwLock};
pub use uexplorer_protocol::{
    ObjectHandle, SnapshotCursor, SnapshotObjectKind, SnapshotPage, SnapshotRecord,
};
use uexplorer_protocol::{
    MAX_GENERATION, MAX_SNAPSHOT_NAME_BYTES, MAX_SNAPSHOT_PAGE_RECORDS, MAX_SNAPSHOT_PATH_BYTES,
    MAX_SNAPSHOT_SOURCE_OBJECTS,
};

const MIN_SEARCH_TOKEN_CHARS: usize = 2;
const MAX_QUERY_SEARCH_BYTES: usize = 1_024;
const MAX_QUERY_SEARCH_TERMS: usize = 16;
const MAX_SEARCH_TOKENS_PER_RECORD: usize = 256;

#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SnapshotQuery {
    pub kind: Option<SnapshotObjectKind>,
    pub full_path: Option<String>,
    pub class_path: Option<String>,
    pub package_path: Option<String>,
    pub address: Option<String>,
    pub search: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SnapshotQueryCursor {
    pub generation: u64,
    pub after_index: i32,
    pub query_fingerprint: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct SnapshotQueryPage {
    pub generation: u64,
    pub context_generation: u64,
    pub source_object_count: u32,
    pub snapshot_record_count: u32,
    pub matched_count: u32,
    pub items: Vec<SnapshotRecord>,
    pub has_more: bool,
    pub next_cursor: Option<SnapshotQueryCursor>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SnapshotCacheError {
    InvalidSession,
    InvalidPage(&'static str),
    RequestCursorMismatch,
    ContextGenerationMismatch { expected: u64, actual: u64 },
    SnapshotGenerationMismatch { expected: u64, actual: u64 },
    IncompleteGeneration { expected: u32, actual: u32 },
    StaleGeneration { current: u64, candidate: u64 },
    DuplicateAddress(String),
    InvalidQuery(&'static str),
    QueryCursorMismatch,
    LockPoisoned,
}

impl SnapshotCacheError {
    pub fn code(&self) -> &'static str {
        match self {
            Self::InvalidSession => "SNAPSHOT_SESSION_INVALID",
            Self::InvalidPage(_) => "SNAPSHOT_PAGE_INVALID",
            Self::RequestCursorMismatch => "SNAPSHOT_CURSOR_MISMATCH",
            Self::ContextGenerationMismatch { .. } => "SNAPSHOT_CONTEXT_MISMATCH",
            Self::SnapshotGenerationMismatch { .. } => "SNAPSHOT_GENERATION_MISMATCH",
            Self::IncompleteGeneration { .. } => "SNAPSHOT_GENERATION_INCOMPLETE",
            Self::StaleGeneration { .. } => "SNAPSHOT_GENERATION_STALE",
            Self::DuplicateAddress(_) => "SNAPSHOT_DUPLICATE_ADDRESS",
            Self::InvalidQuery(_) => "SNAPSHOT_QUERY_INVALID",
            Self::QueryCursorMismatch => "SNAPSHOT_QUERY_CURSOR_MISMATCH",
            Self::LockPoisoned => "SNAPSHOT_CACHE_LOCK_POISONED",
        }
    }
}

impl fmt::Display for SnapshotCacheError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidSession => write!(formatter, "snapshot session ID is invalid"),
            Self::InvalidPage(reason) => write!(formatter, "invalid snapshot page: {reason}"),
            Self::RequestCursorMismatch => {
                write!(
                    formatter,
                    "snapshot page was not fetched with the expected cursor"
                )
            }
            Self::ContextGenerationMismatch { expected, actual } => write!(
                formatter,
                "snapshot context generation mismatch: expected {expected}, got {actual}"
            ),
            Self::SnapshotGenerationMismatch { expected, actual } => write!(
                formatter,
                "snapshot generation mismatch: expected {expected}, got {actual}"
            ),
            Self::IncompleteGeneration { expected, actual } => write!(
                formatter,
                "snapshot generation incomplete: expected {expected} records, got {actual}"
            ),
            Self::StaleGeneration { current, candidate } => write!(
                formatter,
                "snapshot generation is stale: current {current}, candidate {candidate}"
            ),
            Self::DuplicateAddress(address) => {
                write!(formatter, "snapshot contains duplicate address {address}")
            }
            Self::InvalidQuery(reason) => write!(formatter, "invalid snapshot query: {reason}"),
            Self::QueryCursorMismatch => {
                write!(
                    formatter,
                    "query cursor does not belong to this query generation"
                )
            }
            Self::LockPoisoned => write!(formatter, "snapshot cache lock is poisoned"),
        }
    }
}

impl std::error::Error for SnapshotCacheError {}

#[derive(Clone, Debug, Eq, PartialEq)]
struct SnapshotMetadata {
    generation: u64,
    context_generation: u64,
    captured_at_monotonic_us: u64,
    capture_duration_us: u64,
    source_object_count: u32,
    record_count: u32,
    skipped_slots: u32,
}

impl From<&SnapshotPage> for SnapshotMetadata {
    fn from(page: &SnapshotPage) -> Self {
        Self {
            generation: page.generation,
            context_generation: page.context_generation,
            captured_at_monotonic_us: page.captured_at_monotonic_us,
            capture_duration_us: page.capture_duration_us,
            source_object_count: page.source_object_count,
            record_count: page.record_count,
            skipped_slots: page.skipped_slots,
        }
    }
}

pub struct SnapshotAssembler {
    session_id: String,
    metadata: Option<SnapshotMetadata>,
    expected_cursor: Option<SnapshotCursor>,
    records: Vec<SnapshotRecord>,
    completed: bool,
    failed: bool,
}

impl SnapshotAssembler {
    pub fn new(session_id: impl Into<String>) -> Result<Self, SnapshotCacheError> {
        let session_id = session_id.into();
        if !is_valid_session_id(&session_id) {
            return Err(SnapshotCacheError::InvalidSession);
        }
        Ok(Self {
            session_id,
            metadata: None,
            expected_cursor: None,
            records: Vec::new(),
            completed: false,
            failed: false,
        })
    }

    pub fn expected_cursor(&self) -> Option<&SnapshotCursor> {
        self.expected_cursor.as_ref()
    }

    pub fn push_page(
        &mut self,
        request_cursor: Option<&SnapshotCursor>,
        page: SnapshotPage,
    ) -> Result<Option<SnapshotCursor>, SnapshotCacheError> {
        if self.failed {
            return Err(SnapshotCacheError::InvalidPage(
                "assembler is terminal after a rejected page",
            ));
        }
        let result = self.push_page_inner(request_cursor, page);
        if result.is_err() {
            self.failed = true;
        }
        result
    }

    fn push_page_inner(
        &mut self,
        request_cursor: Option<&SnapshotCursor>,
        page: SnapshotPage,
    ) -> Result<Option<SnapshotCursor>, SnapshotCacheError> {
        if self.completed {
            return Err(SnapshotCacheError::InvalidPage(
                "a completed generation cannot accept another page",
            ));
        }
        if request_cursor != self.expected_cursor.as_ref() {
            return Err(SnapshotCacheError::RequestCursorMismatch);
        }
        validate_page_envelope(&self.session_id, &page)?;

        let page_metadata = SnapshotMetadata::from(&page);
        if let Some(metadata) = &self.metadata {
            if page_metadata.context_generation != metadata.context_generation {
                return Err(SnapshotCacheError::ContextGenerationMismatch {
                    expected: metadata.context_generation,
                    actual: page_metadata.context_generation,
                });
            }
            if page_metadata.generation != metadata.generation {
                return Err(SnapshotCacheError::SnapshotGenerationMismatch {
                    expected: metadata.generation,
                    actual: page_metadata.generation,
                });
            }
            if &page_metadata != metadata {
                return Err(SnapshotCacheError::InvalidPage(
                    "snapshot-wide metadata changed between pages",
                ));
            }
        } else {
            self.metadata = Some(page_metadata);
        }

        let previous_index = self.records.last().map(|record| record.handle.index);
        if let (Some(cursor), Some(first)) = (request_cursor, page.items.first()) {
            if first.handle.index <= cursor.after_index {
                return Err(SnapshotCacheError::InvalidPage(
                    "continuation repeated or moved behind its cursor",
                ));
            }
        }
        if let (Some(previous), Some(first)) = (previous_index, page.items.first()) {
            if first.handle.index <= previous {
                return Err(SnapshotCacheError::InvalidPage(
                    "record indexes are not strictly ordered across pages",
                ));
            }
        }

        let next_count = self
            .records
            .len()
            .checked_add(page.items.len())
            .ok_or(SnapshotCacheError::InvalidPage("record count overflow"))?;
        if next_count > page.record_count as usize {
            return Err(SnapshotCacheError::InvalidPage(
                "pages contain more records than record_count",
            ));
        }
        if page.has_more && next_count >= page.record_count as usize {
            return Err(SnapshotCacheError::InvalidPage(
                "has_more is true after the complete record count was returned",
            ));
        }
        if !page.has_more && next_count != page.record_count as usize {
            return Err(SnapshotCacheError::IncompleteGeneration {
                expected: page.record_count,
                actual: next_count as u32,
            });
        }

        self.records.extend(page.items);
        self.expected_cursor = page.next_cursor;
        self.completed = !page.has_more;
        Ok(self.expected_cursor.clone())
    }

    pub fn finish(self) -> Result<SnapshotIndex, SnapshotCacheError> {
        if self.failed {
            return Err(SnapshotCacheError::InvalidPage(
                "assembler is terminal after a rejected page",
            ));
        }
        let metadata = self
            .metadata
            .ok_or(SnapshotCacheError::InvalidPage("generation has no pages"))?;
        if !self.completed {
            return Err(SnapshotCacheError::IncompleteGeneration {
                expected: metadata.record_count,
                actual: self.records.len() as u32,
            });
        }
        SnapshotIndex::build(self.session_id, metadata, self.records)
    }
}

pub struct SnapshotIndex {
    session_id: String,
    metadata: SnapshotMetadata,
    records: Vec<SnapshotRecord>,
    by_kind: HashMap<SnapshotObjectKind, Vec<u32>>,
    by_full_path: HashMap<String, Vec<u32>>,
    by_class_path: HashMap<String, Vec<u32>>,
    by_package_path: HashMap<String, Vec<u32>>,
    by_address: HashMap<u64, u32>,
    search_tokens: BTreeMap<String, Vec<u32>>,
}

impl SnapshotIndex {
    fn build(
        session_id: String,
        metadata: SnapshotMetadata,
        records: Vec<SnapshotRecord>,
    ) -> Result<Self, SnapshotCacheError> {
        let mut index = Self {
            session_id,
            metadata,
            records,
            by_kind: HashMap::new(),
            by_full_path: HashMap::new(),
            by_class_path: HashMap::new(),
            by_package_path: HashMap::new(),
            by_address: HashMap::new(),
            search_tokens: BTreeMap::new(),
        };

        for (position, record) in index.records.iter().enumerate() {
            let position = position as u32;
            let address = parse_canonical_hex(&record.handle.address, false).ok_or(
                SnapshotCacheError::InvalidPage("record address is not canonical hexadecimal"),
            )?;
            if index.by_address.insert(address, position).is_some() {
                return Err(SnapshotCacheError::DuplicateAddress(
                    record.handle.address.clone(),
                ));
            }

            index.by_kind.entry(record.kind).or_default().push(position);
            index
                .by_full_path
                .entry(normalize_key(&record.full_path))
                .or_default()
                .push(position);
            index
                .by_class_path
                .entry(normalize_key(&record.class_path))
                .or_default()
                .push(position);
            index
                .by_package_path
                .entry(normalize_key(&record.package_path))
                .or_default()
                .push(position);

            let mut unique_tokens = BTreeSet::new();
            for value in [
                &record.name,
                &record.full_path,
                &record.class_path,
                &record.package_path,
            ] {
                unique_tokens.extend(
                    tokenize(value)
                        .into_iter()
                        .filter(|token| token.chars().count() >= MIN_SEARCH_TOKEN_CHARS),
                );
                if unique_tokens.len() > MAX_SEARCH_TOKENS_PER_RECORD {
                    return Err(SnapshotCacheError::InvalidPage(
                        "record exceeds the bounded search-token index limit",
                    ));
                }
            }
            for token in unique_tokens {
                index.search_tokens.entry(token).or_default().push(position);
            }
        }
        Ok(index)
    }

    pub fn record_count(&self) -> u32 {
        self.metadata.record_count
    }

    pub fn session_id(&self) -> &str {
        &self.session_id
    }

    pub fn generation(&self) -> u64 {
        self.metadata.generation
    }

    pub fn context_generation(&self) -> u64 {
        self.metadata.context_generation
    }

    pub fn records(&self) -> &[SnapshotRecord] {
        &self.records
    }

    pub fn count_by_kind(&self, kind: SnapshotObjectKind) -> u32 {
        self.by_kind
            .get(&kind)
            .map_or(0, |positions| positions.len() as u32)
    }

    pub fn object_by_index(&self, object_index: i32) -> Option<&SnapshotRecord> {
        self.records
            .binary_search_by_key(&object_index, |record| record.handle.index)
            .ok()
            .map(|position| &self.records[position])
    }

    pub fn object_by_address(&self, address: &str) -> Option<&SnapshotRecord> {
        let address = parse_canonical_hex(address, false)?;
        self.by_address
            .get(&address)
            .map(|position| &self.records[*position as usize])
    }

    pub fn query(
        &self,
        query: &SnapshotQuery,
        cursor: Option<&SnapshotQueryCursor>,
        limit: usize,
    ) -> Result<SnapshotQueryPage, SnapshotCacheError> {
        if !(1..=MAX_SNAPSHOT_PAGE_RECORDS).contains(&limit) {
            return Err(SnapshotCacheError::InvalidQuery(
                "limit must be in range 1..128",
            ));
        }
        let fingerprint = query_fingerprint(query)?;
        if let Some(cursor) = cursor {
            if cursor.generation != self.metadata.generation
                || cursor.after_index < 0
                || cursor.query_fingerprint != fingerprint
            {
                return Err(SnapshotCacheError::QueryCursorMismatch);
            }
        }

        let search_candidates = match &query.search {
            Some(search) => Some(self.search_candidates(search)?),
            None => None,
        };
        let address_candidate = match &query.address {
            Some(address) => {
                let parsed = parse_canonical_hex(address, false).ok_or(
                    SnapshotCacheError::InvalidQuery("address must be canonical hexadecimal"),
                )?;
                Some(
                    self.by_address
                        .get(&parsed)
                        .copied()
                        .map(|position| vec![position])
                        .unwrap_or_default(),
                )
            }
            None => None,
        };

        let mut candidate_lists: Vec<&[u32]> = Vec::new();
        if let Some(kind) = query.kind {
            candidate_lists.push(self.by_kind.get(&kind).map(Vec::as_slice).unwrap_or(&[]));
        }
        push_key_candidates(
            &mut candidate_lists,
            &self.by_full_path,
            query.full_path.as_deref(),
        )?;
        push_key_candidates(
            &mut candidate_lists,
            &self.by_class_path,
            query.class_path.as_deref(),
        )?;
        push_key_candidates(
            &mut candidate_lists,
            &self.by_package_path,
            query.package_path.as_deref(),
        )?;
        if let Some(candidates) = search_candidates.as_deref() {
            candidate_lists.push(candidates);
        }
        if let Some(candidates) = address_candidate.as_deref() {
            candidate_lists.push(candidates);
        }

        if candidate_lists.is_empty() {
            return Ok(self.query_all(cursor, limit, fingerprint));
        }
        candidate_lists.sort_unstable_by_key(|candidates| candidates.len());
        let positions: Cow<'_, [u32]> = if candidate_lists.len() == 1 {
            Cow::Borrowed(candidate_lists[0])
        } else {
            let mut intersection = candidate_lists[0].to_vec();
            for candidates in &candidate_lists[1..] {
                intersection.retain(|position| candidates.binary_search(position).is_ok());
                if intersection.is_empty() {
                    break;
                }
            }
            Cow::Owned(intersection)
        };
        Ok(self.page_positions(&positions, cursor, limit, fingerprint))
    }

    fn search_candidates(&self, search: &str) -> Result<Vec<u32>, SnapshotCacheError> {
        let terms = validate_search_terms(search)?;

        let mut intersection: Option<Vec<u32>> = None;
        for term in terms {
            let mut matches = Vec::new();
            for (token, positions) in self.search_tokens.range(term.clone()..) {
                if !token.starts_with(&term) {
                    break;
                }
                matches.extend_from_slice(positions);
            }
            matches.sort_unstable();
            matches.dedup();
            intersection = Some(match intersection {
                None => matches,
                Some(mut current) => {
                    current.retain(|position| matches.binary_search(position).is_ok());
                    current
                }
            });
            if intersection.as_ref().is_some_and(Vec::is_empty) {
                break;
            }
        }
        Ok(intersection.unwrap_or_default())
    }

    fn query_all(
        &self,
        cursor: Option<&SnapshotQueryCursor>,
        limit: usize,
        fingerprint: String,
    ) -> SnapshotQueryPage {
        let after_index = cursor.map_or(-1, |value| value.after_index);
        let start = self
            .records
            .partition_point(|record| record.handle.index <= after_index);
        let end = (start + limit).min(self.records.len());
        let items = self.records[start..end].to_vec();
        self.make_query_page(
            items,
            self.records.len(),
            end < self.records.len(),
            fingerprint,
        )
    }

    fn page_positions(
        &self,
        positions: &[u32],
        cursor: Option<&SnapshotQueryCursor>,
        limit: usize,
        fingerprint: String,
    ) -> SnapshotQueryPage {
        let after_index = cursor.map_or(-1, |value| value.after_index);
        let start = positions.partition_point(|position| {
            self.records[*position as usize].handle.index <= after_index
        });
        let end = (start + limit).min(positions.len());
        let items = positions[start..end]
            .iter()
            .map(|position| self.records[*position as usize].clone())
            .collect();
        self.make_query_page(items, positions.len(), end < positions.len(), fingerprint)
    }

    fn make_query_page(
        &self,
        items: Vec<SnapshotRecord>,
        matched_count: usize,
        has_more: bool,
        fingerprint: String,
    ) -> SnapshotQueryPage {
        let next_cursor = if has_more {
            items.last().map(|record| SnapshotQueryCursor {
                generation: self.metadata.generation,
                after_index: record.handle.index,
                query_fingerprint: fingerprint,
            })
        } else {
            None
        };
        SnapshotQueryPage {
            generation: self.metadata.generation,
            context_generation: self.metadata.context_generation,
            source_object_count: self.metadata.source_object_count,
            snapshot_record_count: self.metadata.record_count,
            matched_count: matched_count as u32,
            items,
            has_more,
            next_cursor,
        }
    }
}

pub struct SnapshotCache {
    session_id: String,
    current: RwLock<Option<Arc<SnapshotIndex>>>,
}

impl SnapshotCache {
    pub fn new(session_id: impl Into<String>) -> Result<Self, SnapshotCacheError> {
        let session_id = session_id.into();
        if !is_valid_session_id(&session_id) {
            return Err(SnapshotCacheError::InvalidSession);
        }
        Ok(Self {
            session_id,
            current: RwLock::new(None),
        })
    }

    pub fn assembler(&self) -> Result<SnapshotAssembler, SnapshotCacheError> {
        SnapshotAssembler::new(self.session_id.clone())
    }

    pub fn publish(&self, index: SnapshotIndex) -> Result<Arc<SnapshotIndex>, SnapshotCacheError> {
        if index.session_id != self.session_id {
            return Err(SnapshotCacheError::InvalidSession);
        }
        let mut current = self
            .current
            .write()
            .map_err(|_| SnapshotCacheError::LockPoisoned)?;
        if let Some(published) = current.as_ref() {
            if index.metadata.context_generation != published.metadata.context_generation {
                return Err(SnapshotCacheError::ContextGenerationMismatch {
                    expected: published.metadata.context_generation,
                    actual: index.metadata.context_generation,
                });
            }
            if index.metadata.generation <= published.metadata.generation {
                return Err(SnapshotCacheError::StaleGeneration {
                    current: published.metadata.generation,
                    candidate: index.metadata.generation,
                });
            }
        }

        let published = Arc::new(index);
        *current = Some(Arc::clone(&published));
        Ok(published)
    }

    pub fn current(&self) -> Result<Option<Arc<SnapshotIndex>>, SnapshotCacheError> {
        self.current
            .read()
            .map(|current| current.clone())
            .map_err(|_| SnapshotCacheError::LockPoisoned)
    }
}

fn validate_page_envelope(session_id: &str, page: &SnapshotPage) -> Result<(), SnapshotCacheError> {
    if page.generation == 0
        || page.generation > MAX_GENERATION
        || page.context_generation == 0
        || page.context_generation > MAX_GENERATION
        || page.captured_at_monotonic_us == 0
    {
        return Err(SnapshotCacheError::InvalidPage(
            "generation, context, or capture timestamp is outside the protocol range",
        ));
    }
    if page.source_object_count > MAX_SNAPSHOT_SOURCE_OBJECTS
        || page.record_count > page.source_object_count
        || page.skipped_slots != page.source_object_count - page.record_count
    {
        return Err(SnapshotCacheError::InvalidPage(
            "snapshot source, record, and skipped counts are inconsistent",
        ));
    }
    if page.items.len() > MAX_SNAPSHOT_PAGE_RECORDS {
        return Err(SnapshotCacheError::InvalidPage(
            "page exceeds the 128-record protocol limit",
        ));
    }
    if page.has_more && page.items.is_empty() {
        return Err(SnapshotCacheError::InvalidPage(
            "an empty page cannot advertise a continuation",
        ));
    }

    let expected_next = page.items.last().map(|record| SnapshotCursor {
        generation: page.generation,
        after_index: record.handle.index,
    });
    if page.has_more {
        if page.next_cursor != expected_next {
            return Err(SnapshotCacheError::InvalidPage(
                "continuation cursor does not identify the last returned record",
            ));
        }
    } else if page.next_cursor.is_some() {
        return Err(SnapshotCacheError::InvalidPage(
            "final page must return a null continuation cursor",
        ));
    }

    let mut previous_index = -1;
    for record in &page.items {
        if record.handle.session_id != session_id
            || record.handle.context_generation != page.context_generation
            || record.handle.index < 0
            || record.handle.index as u32 >= page.source_object_count
            || record.handle.serial <= 0
            || !parse_canonical_hex(&record.handle.address, false).is_some_and(|value| value != 0)
            || !parse_canonical_hex(&record.handle.class_fingerprint, true)
                .is_some_and(|value| value != 0)
            || record.handle.index <= previous_index
            || !is_valid_text(&record.name, MAX_SNAPSHOT_NAME_BYTES)
            || !is_valid_text(&record.full_path, MAX_SNAPSHOT_PATH_BYTES)
            || !is_valid_text(&record.class_path, MAX_SNAPSHOT_PATH_BYTES)
            || !is_valid_text(&record.package_path, MAX_SNAPSHOT_PATH_BYTES)
        {
            return Err(SnapshotCacheError::InvalidPage(
                "record identity, ordering, or metadata is invalid",
            ));
        }
        previous_index = record.handle.index;
    }
    Ok(())
}

fn push_key_candidates<'a>(
    candidates: &mut Vec<&'a [u32]>,
    index: &'a HashMap<String, Vec<u32>>,
    key: Option<&str>,
) -> Result<(), SnapshotCacheError> {
    if let Some(key) = key {
        if !is_valid_text(key, MAX_SNAPSHOT_PATH_BYTES) {
            return Err(SnapshotCacheError::InvalidQuery(
                "path filters must be non-empty and at most 4096 bytes",
            ));
        }
        candidates.push(
            index
                .get(&normalize_key(key))
                .map(Vec::as_slice)
                .unwrap_or(&[]),
        );
    }
    Ok(())
}

fn query_fingerprint(query: &SnapshotQuery) -> Result<String, SnapshotCacheError> {
    if let Some(search) = &query.search {
        validate_search_terms(search)?;
    }
    if let Some(address) = &query.address {
        if parse_canonical_hex(address, false).is_none() {
            return Err(SnapshotCacheError::InvalidQuery(
                "address must be canonical hexadecimal",
            ));
        }
    }
    for path in [&query.full_path, &query.class_path, &query.package_path]
        .into_iter()
        .flatten()
    {
        if !is_valid_text(path, MAX_SNAPSHOT_PATH_BYTES) {
            return Err(SnapshotCacheError::InvalidQuery(
                "path filters must be non-empty and at most 4096 bytes",
            ));
        }
    }

    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    hash_field(
        &mut hash,
        query
            .kind
            .map(|kind| kind as u8)
            .as_ref()
            .map(u8::to_string)
            .as_deref(),
    );
    hash_field(
        &mut hash,
        query.full_path.as_deref().map(normalize_key).as_deref(),
    );
    hash_field(
        &mut hash,
        query.class_path.as_deref().map(normalize_key).as_deref(),
    );
    hash_field(
        &mut hash,
        query.package_path.as_deref().map(normalize_key).as_deref(),
    );
    hash_field(
        &mut hash,
        query.address.as_deref().map(normalize_key).as_deref(),
    );
    let normalized_search = query
        .search
        .as_deref()
        .map(tokenize)
        .map(|tokens| tokens.join(" "));
    hash_field(&mut hash, normalized_search.as_deref());
    Ok(format!("{hash:016X}"))
}

fn hash_field(hash: &mut u64, value: Option<&str>) {
    let bytes = value.unwrap_or("").as_bytes();
    for byte in (bytes.len() as u64).to_le_bytes().iter().chain(bytes) {
        *hash ^= u64::from(*byte);
        *hash = hash.wrapping_mul(0x100_0000_01B3);
    }
}

fn validate_search_terms(search: &str) -> Result<Vec<String>, SnapshotCacheError> {
    if search.is_empty()
        || search.len() > MAX_QUERY_SEARCH_BYTES
        || search.chars().any(|character| character.is_control())
    {
        return Err(SnapshotCacheError::InvalidQuery(
            "search must be 1..1024 bytes without control characters",
        ));
    }
    let terms = tokenize(search);
    if terms.is_empty()
        || terms.len() > MAX_QUERY_SEARCH_TERMS
        || terms
            .iter()
            .any(|term| term.chars().count() < MIN_SEARCH_TOKEN_CHARS)
    {
        return Err(SnapshotCacheError::InvalidQuery(
            "search must contain 1..16 tokens of at least two characters",
        ));
    }
    Ok(terms)
}

fn tokenize(value: &str) -> Vec<String> {
    let chars: Vec<char> = value.chars().collect();
    let mut tokens = Vec::new();
    let mut current = String::new();
    for (index, character) in chars.iter().copied().enumerate() {
        if !character.is_alphanumeric() {
            flush_token(&mut tokens, &mut current);
            continue;
        }
        let previous = index.checked_sub(1).and_then(|value| chars.get(value));
        let next = chars.get(index + 1);
        let camel_boundary = !current.is_empty()
            && ((character.is_uppercase()
                && previous.is_some_and(|value| value.is_lowercase() || value.is_numeric()))
                || (character.is_uppercase()
                    && previous.is_some_and(|value| value.is_uppercase())
                    && next.is_some_and(|value| value.is_lowercase())));
        if camel_boundary {
            flush_token(&mut tokens, &mut current);
        }
        current.extend(character.to_lowercase());
    }
    flush_token(&mut tokens, &mut current);
    tokens
}

fn flush_token(tokens: &mut Vec<String>, current: &mut String) {
    if !current.is_empty() {
        tokens.push(std::mem::take(current));
    }
}

fn normalize_key(value: &str) -> String {
    value.to_lowercase()
}

fn parse_canonical_hex(value: &str, exact_16_digits: bool) -> Option<u64> {
    let digits = if exact_16_digits {
        value
    } else {
        value.strip_prefix("0x")?
    };
    if digits.is_empty()
        || digits.len() > 16
        || (exact_16_digits && digits.len() != 16)
        || !digits
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'A'..=b'F').contains(&byte))
    {
        return None;
    }
    u64::from_str_radix(digits, 16).ok()
}

fn is_valid_session_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}

fn is_valid_text(value: &str, max_bytes: usize) -> bool {
    !value.is_empty()
        && value.len() <= max_bytes
        && !value.chars().any(|character| character.is_control())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde::Deserialize;

    fn record(
        index: i32,
        name: &str,
        class_path: &str,
        package_path: &str,
        kind: SnapshotObjectKind,
    ) -> SnapshotRecord {
        SnapshotRecord {
            handle: ObjectHandle {
                session_id: "fixture-session-4242".to_string(),
                context_generation: 42,
                index,
                serial: index + 101,
                address: format!("0x{:016X}", 0x1401_0000u64 + index as u64 * 0x100),
                class_fingerprint: format!("{:016X}", 0xA100u64 + index as u64),
            },
            name: name.to_string(),
            full_path: format!("Object /Game/Fixture.{name}"),
            class_path: class_path.to_string(),
            package_path: package_path.to_string(),
            kind,
        }
    }

    fn pages(generation: u64) -> Vec<(Option<SnapshotCursor>, SnapshotPage)> {
        let first_items = vec![
            record(
                0,
                "BP_PlayerCharacter",
                "Class /Game/Player.Character",
                "Package /Game/Player",
                SnapshotObjectKind::Object,
            ),
            record(
                2,
                "EnemyCharacter",
                "Class /Game/Enemy.Character",
                "Package /Game/Enemy",
                SnapshotObjectKind::Object,
            ),
        ];
        let continuation = SnapshotCursor {
            generation,
            after_index: 2,
        };
        vec![
            (
                None,
                SnapshotPage {
                    generation,
                    context_generation: 42,
                    captured_at_monotonic_us: 1_000_000,
                    capture_duration_us: 2_500,
                    source_object_count: 4,
                    record_count: 3,
                    skipped_slots: 1,
                    items: first_items,
                    has_more: true,
                    next_cursor: Some(continuation.clone()),
                },
            ),
            (
                Some(continuation),
                SnapshotPage {
                    generation,
                    context_generation: 42,
                    captured_at_monotonic_us: 1_000_000,
                    capture_duration_us: 2_500,
                    source_object_count: 4,
                    record_count: 3,
                    skipped_slots: 1,
                    items: vec![record(
                        3,
                        "PlayerController",
                        "Class /Script/Engine.PlayerController",
                        "Package /Game/Player",
                        SnapshotObjectKind::Class,
                    )],
                    has_more: false,
                    next_cursor: None,
                },
            ),
        ]
    }

    fn assemble(generation: u64) -> SnapshotIndex {
        let mut assembler = SnapshotAssembler::new("fixture-session-4242").unwrap();
        for (cursor, page) in pages(generation) {
            assembler.push_page(cursor.as_ref(), page).unwrap();
        }
        assembler.finish().unwrap()
    }

    #[test]
    fn complete_generation_is_published_atomically_and_indexed() {
        let cache = SnapshotCache::new("fixture-session-4242").unwrap();
        assert!(cache.current().unwrap().is_none());

        let published = cache.publish(assemble(9)).unwrap();
        assert_eq!(published.generation(), 9);
        assert_eq!(published.context_generation(), 42);
        assert_eq!(published.records().len(), 3);
        assert_eq!(published.object_by_index(2).unwrap().name, "EnemyCharacter");
        assert_eq!(
            published
                .object_by_address("0x0000000014010200")
                .unwrap()
                .name,
            "EnemyCharacter"
        );
        assert_eq!(cache.current().unwrap().unwrap().generation(), 9);
    }

    #[test]
    fn mixed_or_incomplete_generation_never_replaces_current() {
        let cache = SnapshotCache::new("fixture-session-4242").unwrap();
        cache.publish(assemble(9)).unwrap();

        let mut assembler = cache.assembler().unwrap();
        let mut candidate_pages = pages(10);
        let (cursor, first) = candidate_pages.remove(0);
        assembler.push_page(cursor.as_ref(), first).unwrap();
        let (cursor, mut second) = candidate_pages.remove(0);
        second.generation = 11;
        assert!(matches!(
            assembler.push_page(cursor.as_ref(), second),
            Err(SnapshotCacheError::SnapshotGenerationMismatch {
                expected: 10,
                actual: 11
            })
        ));
        assert_eq!(cache.current().unwrap().unwrap().generation(), 9);

        let mut incomplete = cache.assembler().unwrap();
        let (_, first) = pages(10).remove(0);
        incomplete.push_page(None, first).unwrap();
        assert!(matches!(
            incomplete.finish(),
            Err(SnapshotCacheError::IncompleteGeneration {
                expected: 3,
                actual: 2
            })
        ));
        assert_eq!(cache.current().unwrap().unwrap().generation(), 9);
        assert!(matches!(
            cache.publish(assemble(9)),
            Err(SnapshotCacheError::StaleGeneration {
                current: 9,
                candidate: 9
            })
        ));
    }

    #[test]
    fn indexed_queries_have_exact_totals_and_query_bound_cursors() {
        let index = assemble(9);
        let first = index.query(&SnapshotQuery::default(), None, 2).unwrap();
        assert_eq!(first.snapshot_record_count, 3);
        assert_eq!(first.matched_count, 3);
        assert_eq!(first.items.len(), 2);
        assert!(first.has_more);
        let final_page = index
            .query(&SnapshotQuery::default(), first.next_cursor.as_ref(), 2)
            .unwrap();
        assert_eq!(final_page.items.len(), 1);
        assert!(!final_page.has_more);

        let searched = index
            .query(
                &SnapshotQuery {
                    search: Some("player char".to_string()),
                    ..SnapshotQuery::default()
                },
                None,
                128,
            )
            .unwrap();
        assert_eq!(searched.matched_count, 1);
        assert_eq!(searched.items[0].name, "BP_PlayerCharacter");

        let classes = index
            .query(
                &SnapshotQuery {
                    kind: Some(SnapshotObjectKind::Class),
                    ..SnapshotQuery::default()
                },
                None,
                128,
            )
            .unwrap();
        assert_eq!(classes.matched_count, 1);
        assert_eq!(classes.items[0].name, "PlayerController");

        let package = index
            .query(
                &SnapshotQuery {
                    package_path: Some("package /game/player".to_string()),
                    ..SnapshotQuery::default()
                },
                None,
                128,
            )
            .unwrap();
        assert_eq!(package.matched_count, 2);

        let wrong_query = SnapshotQuery {
            kind: Some(SnapshotObjectKind::Object),
            ..SnapshotQuery::default()
        };
        assert_eq!(
            index.query(&wrong_query, first.next_cursor.as_ref(), 2),
            Err(SnapshotCacheError::QueryCursorMismatch)
        );
        assert_eq!(
            index.query(
                &SnapshotQuery {
                    search: Some("a".to_string()),
                    ..SnapshotQuery::default()
                },
                None,
                2
            ),
            Err(SnapshotCacheError::InvalidQuery(
                "search must contain 1..16 tokens of at least two characters"
            ))
        );
    }

    #[test]
    fn malformed_page_and_noncanonical_identity_are_rejected() {
        let mut invalid = pages(9).remove(0).1;
        invalid.items[0].handle.address = "0x14010000junk".to_string();
        let mut assembler = SnapshotAssembler::new("fixture-session-4242").unwrap();
        assert_eq!(
            assembler.push_page(None, invalid),
            Err(SnapshotCacheError::InvalidPage(
                "record identity, ordering, or metadata is invalid"
            ))
        );
        assert_eq!(
            assembler.push_page(None, pages(9).remove(0).1),
            Err(SnapshotCacheError::InvalidPage(
                "assembler is terminal after a rejected page"
            ))
        );

        let mut bad_cursor = pages(9).remove(0).1;
        bad_cursor.next_cursor.as_mut().unwrap().after_index = 1;
        let mut cursor_assembler = SnapshotAssembler::new("fixture-session-4242").unwrap();
        assert!(matches!(
            cursor_assembler.push_page(None, bad_cursor),
            Err(SnapshotCacheError::InvalidPage(_))
        ));

        let mut zero_identity = pages(9).remove(0).1;
        zero_identity.items[0].handle.address = "0x0".to_string();
        let mut identity_assembler = SnapshotAssembler::new("fixture-session-4242").unwrap();
        assert!(matches!(
            identity_assembler.push_page(None, zero_identity),
            Err(SnapshotCacheError::InvalidPage(_))
        ));
    }

    #[test]
    fn announced_record_count_does_not_trigger_eager_bulk_allocation() {
        let mut page = pages(9).remove(0).1;
        page.source_object_count = MAX_SNAPSHOT_SOURCE_OBJECTS;
        page.record_count = MAX_SNAPSHOT_SOURCE_OBJECTS;
        page.skipped_slots = 0;
        page.items.truncate(1);
        page.next_cursor = Some(SnapshotCursor {
            generation: 9,
            after_index: 0,
        });

        let mut assembler = SnapshotAssembler::new("fixture-session-4242").unwrap();
        assembler.push_page(None, page).unwrap();
        assert!(assembler.records.capacity() <= MAX_SNAPSHOT_PAGE_RECORDS);
    }

    #[test]
    fn golden_snapshot_page_response_deserializes_strictly() {
        #[derive(Deserialize)]
        struct Envelope {
            data: SnapshotPage,
        }

        let envelope: Envelope = serde_json::from_str(include_str!(
            "../../../../protocol/v1/fixtures/object-snapshot-page-response.json"
        ))
        .unwrap();
        let mut assembler = SnapshotAssembler::new("fixture-session-4242").unwrap();
        let next = assembler.push_page(None, envelope.data).unwrap().unwrap();
        assert_eq!(next.generation, 9);
        assert_eq!(next.after_index, 1);

        let unknown_field = r#"{
            "generation":9,"after_index":1,"unexpected":true
        }"#;
        assert!(serde_json::from_str::<SnapshotCursor>(unknown_field).is_err());
    }
}
