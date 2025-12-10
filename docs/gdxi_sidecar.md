# GDXI Sidecar (Draft)

Purpose: persist lightweight metadata per GDX so subsequent opens (or process restarts) can skip expensive scans.

Contents (per file):

- header: version, source_path, file_size, mtime/etag, hash_hint (small head/tail hash), build/tool version
- symbols[]: name, type_code, dimension_count, record_count, data_position, domain_labels
- optional: cached_domain_values per symbol (vector of unique values per dimension) when requested

Staleness check: match size+mtime (or etag for remote); optionally verify hash_hint; on mismatch ignore file.

In-memory reuse: keep a per-process cache keyed by resolved_path that survives close/reopen inside the same process.

Auto-build policy: only when missing, local, size > threshold; otherwise manual/flag.