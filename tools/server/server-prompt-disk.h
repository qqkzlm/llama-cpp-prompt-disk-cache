#pragma once

#include "server-task.h"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

// content hash of a token sequence (FNV-1a 64)
uint64_t server_prompt_disk_hash(const llama_tokens & tokens);

std::string server_prompt_file_fingerprint(const std::string & path);

// guards: a disk entry is usable only when all of these match the running server
struct server_prompt_disk_guard {
    std::string model_fingerprint;
    std::string legacy_model_path;
    int32_t n_ctx = 0;
    int32_t flash_attn = 0;
    int32_t cache_k = 0;
    int32_t cache_v = 0;
    std::string build;
};

// persistent L2 behind server_prompt_cache (RAM).
// Two entry kinds, both keyed by token hash:
// - full entries: whole prompt tokens + all checkpoints + full seq state (final).
// - prefix entries (prefix-only mode): token prefix [0, N) + full seq state at N.
//   Each prefix checkpoint stores a COMPLETE restorable KV snapshot, not a partial
//   delta, plus the token prefix up to that point. Lookup is longest-prefix match
//   over complete checkpoints, so a new session reuses the longest common prefix
//   and only recomputes tokens after the fork point. Prefix-only omits the session
//   tail (no final full-prompt entry) but every stored checkpoint is self-sufficient.
struct server_prompt_disk {
    server_prompt_disk(
        const std::string & dir,
        uint64_t budget_bytes,
        size_t min_tokens,
        const server_prompt_disk_guard & guard);

    ~server_prompt_disk();

    server_prompt_disk(const server_prompt_disk &) = delete;
    server_prompt_disk & operator=(const server_prompt_disk &) = delete;

    // scan dir, index headers + token lists (blobs stay on disk until load)
    void scan();

    // copy entry and queue a background write. drops exact duplicates. thread-safe.
    void store(
        const server_tokens & tokens,
        const std::list<common_prompt_checkpoint> & checkpoints,
        const std::vector<uint8_t> & data_main,
        const std::vector<uint8_t> & data_drft) const;

    void store_prefix_checkpoint(
        const server_tokens & tokens,
        const common_prompt_checkpoint & checkpoint,
        size_t prefix_limit,
        const std::vector<uint8_t> & data_main,
        const std::vector<uint8_t> & data_drft) const;

    // longest-prefix match over the index. returns false on miss.
    bool find_best(const server_tokens & query, std::string & out_path, int & out_lcp) const;

    // read the whole file (ms_read), then parse it (ms_parse)
    bool load_entry(
        const std::string & path,
        server_prompt & prompt_out,
        server_prompt_data & data_out,
        double & ms_read,
        double & ms_parse) const;

    size_t entry_count() const;

private:
    struct index_entry {
        uint64_t hash;
        std::string path;
        llama_tokens tokens; // for prefix compare (full prompt or token prefix)
        std::vector<int64_t> ckpt_ntok; // checkpoint n_tokens list, for usable-length
        std::vector<int32_t> ckpt_max; // legacy checkpoint pos_max list (v1/v2 partial entries)
        bool is_prefix = false; // true = prefix-only entry (tokens are a prefix, not a full prompt)
        bool has_full_state = false; // true = data_main holds a complete restorable seq state
        uint32_t format_version = 0;
        uint64_t bytes = 0;
    };

    struct queue_item {
        llama_tokens tokens;
        std::list<common_prompt_checkpoint> checkpoints;
        std::vector<uint8_t> data_main;
        std::vector<uint8_t> data_drft;
        bool checkpoint_only = false;
    };

    void worker_main() const;
    void write_entry(const queue_item & item) const;
    void enforce_budget() const;
    std::string path_for(uint64_t hash) const;

    std::string dir;
    uint64_t budget_bytes;
    size_t min_tokens;
    server_prompt_disk_guard guard;

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    mutable std::vector<index_entry> index; // guarded by mutex
    mutable uint64_t index_bytes = 0;       // guarded by mutex
    uint64_t guard_hash = 0;

    mutable std::list<queue_item> queue; // guarded by mutex
    mutable bool stop = false;           // guarded by mutex
    std::thread worker;
};
