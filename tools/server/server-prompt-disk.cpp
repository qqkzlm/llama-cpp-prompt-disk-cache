#include "server-prompt-disk.h"

#include "server-common.h"
#include "gguf.h"
#include "hash/hash.h"

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <cctype>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static const uint32_t PDC_MAGIC = 0x4543504c; // "LPCE"
static const uint32_t PDC_VER = 3;

std::string server_prompt_file_fingerprint(const std::string & path) {
    std::error_code ec;
    const uint64_t file_size = fs::file_size(path, ec);
    if (ec || file_size == 0) {
        throw std::runtime_error("cannot get model file size for prompt cache fingerprint: " + path);
    }

    struct gguf_init_params params = { true, nullptr };
    struct gguf_context * gguf = gguf_init_from_file(path.c_str(), params);
    if (gguf == nullptr) {
        throw std::runtime_error("cannot read GGUF metadata for prompt cache fingerprint: " + path);
    }

    std::ostringstream metadata;
    metadata << gguf_get_version(gguf) << ':' << gguf_get_n_kv(gguf) << ':' << gguf_get_n_tensors(gguf);
    const char * keys[] = {
        "general.architecture",
        "general.name",
        "general.file_type",
        "tokenizer.ggml.model",
        "tokenizer.ggml.pre",
    };
    for (const char * key : keys) {
        const int64_t id = gguf_find_key(gguf, key);
        metadata << ':' << key << '=';
        if (id >= 0) {
            switch (gguf_get_kv_type(gguf, id)) {
                case GGUF_TYPE_STRING:
                    metadata << gguf_get_val_str(gguf, id);
                    break;
                case GGUF_TYPE_UINT32:
                    metadata << gguf_get_val_u32(gguf, id);
                    break;
                case GGUF_TYPE_INT32:
                    metadata << gguf_get_val_i32(gguf, id);
                    break;
                default:
                    metadata << "type" << (int) gguf_get_kv_type(gguf, id);
                    break;
            }
        }
    }
    gguf_free(gguf);

    constexpr size_t sample_size = 16 * 1024 * 1024;
    const size_t actual_sample_size = (size_t) std::min<uint64_t>(file_size, sample_size);
    const size_t sample_count = file_size > actual_sample_size * 2 ? 3 : 1;
    std::vector<uint8_t> sample(actual_sample_size * sample_count);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open model file for prompt cache fingerprint: " + path);
    }
    const uint64_t positions[] = {
        0,
        sample_count == 3 ? (file_size - actual_sample_size) / 2 : 0,
        sample_count == 3 ? file_size - actual_sample_size : 0,
    };
    for (size_t i = 0; i < sample_count; i++) {
        file.seekg((std::streamoff) positions[i], std::ios::beg);
        file.read((char *) sample.data() + i * actual_sample_size, actual_sample_size);
        if ((size_t) file.gcount() != actual_sample_size) {
            throw std::runtime_error("cannot read model sample for prompt cache fingerprint: " + path);
        }
    }

    const std::string metadata_text = metadata.str();
    const std::string metadata_hash = hash_sha256_hex(metadata_text.data(), metadata_text.size());
    const std::string sample_hash = hash_sha256_hex(sample.data(), sample.size());
    return string_format("size=%llu;meta=%s;sample=%s",
            (unsigned long long) file_size, metadata_hash.c_str(), sample_hash.c_str());
}

uint64_t server_prompt_disk_hash(const llama_tokens & tokens) {
    // FNV-1a 64 over raw token bytes
    uint64_t h = 1469598103934665603ull;
    for (const int32_t t : tokens) {
        uint32_t u = (uint32_t) t;
        for (int i = 0; i < 4; i++) {
            h ^= (uint8_t) (u >> (i * 8));
            h *= 1099511628211ull;
        }
    }
    return h;
}

static void wr(std::ofstream & f, const void * p, size_t n) {
    f.write((const char *) p, n);
}

static bool rd(std::ifstream & f, void * p, size_t n) {
    f.read((char *) p, n);
    return (bool) f;
}

static void wr_str(std::ofstream & f, const std::string & s) {
    const uint64_t n = s.size();
    wr(f, &n, sizeof(n));
    if (n) {
        f.write(s.data(), n);
    }
}

static bool rd_str(std::ifstream & f, std::string & s) {
    uint64_t n = 0;
    if (!rd(f, &n, sizeof(n)) || n > 1 * 1024 * 1024) {
        return false;
    }
    s.resize(n);
    return n == 0 || rd(f, s.data(), n);
}

server_prompt_disk::server_prompt_disk(
        const std::string & dir,
        uint64_t budget_bytes,
        size_t min_tokens,
        const server_prompt_disk_guard & guard)
    : dir(dir), budget_bytes(budget_bytes), min_tokens(min_tokens), guard(guard) {
    for (const unsigned char c : guard.model_fingerprint) {
        guard_hash ^= c;
        guard_hash *= 1099511628211ull;
    }
    worker = std::thread([this]() { worker_main(); });
}

server_prompt_disk::~server_prompt_disk() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
    }
    cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

std::string server_prompt_disk::path_for(uint64_t hash) const {
    char name[56];
    snprintf(name, sizeof(name), "%016llx-%016llx.centry",
            (unsigned long long) guard_hash, (unsigned long long) hash);
    return dir + name;
}

static bool same_path(std::string a, std::string b) {
    a = fs::path(a).lexically_normal().generic_string();
    b = fs::path(b).lexically_normal().generic_string();
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

static bool guard_same(const server_prompt_disk_guard & a, const server_prompt_disk_guard & b, uint32_t version) {
    const bool same_model = version == 1
        ? same_path(a.model_fingerprint, b.legacy_model_path)
        : a.model_fingerprint == b.model_fingerprint;
    return same_model
        && a.flash_attn == b.flash_attn
        && a.cache_k == b.cache_k
        && a.cache_v == b.cache_v
        && a.build == b.build;
}

// read header + guard + tokens only, leave the stream positioned at checkpoints
static bool read_entry_head(
        std::ifstream & f,
        server_prompt_disk_guard & guard_out,
        int32_t & n_ctx_out,
        llama_tokens & tokens_out,
        uint32_t & version_out) {
    uint32_t magic = 0, ver = 0;
    if (!rd(f, &magic, sizeof(magic)) || !rd(f, &ver, sizeof(ver))) {
        return false;
    }
    if (magic != PDC_MAGIC || (ver != 1 && ver != 2 && ver != PDC_VER)) {
        return false;
    }
    version_out = ver;
    if (!rd_str(f, guard_out.model_fingerprint)) {
        return false;
    }
    if (!rd(f, &n_ctx_out, sizeof(n_ctx_out))
            || !rd(f, &guard_out.flash_attn, sizeof(guard_out.flash_attn))
            || !rd(f, &guard_out.cache_k, sizeof(guard_out.cache_k))
            || !rd(f, &guard_out.cache_v, sizeof(guard_out.cache_v))) {
        return false;
    }
    if (!rd_str(f, guard_out.build)) {
        return false;
    }
    uint64_t n = 0;
    if (!rd(f, &n, sizeof(n)) || n > (uint64_t) 4 * 1024 * 1024) {
        return false;
    }
    tokens_out.resize(n);
    return n == 0 || rd(f, tokens_out.data(), n * sizeof(int32_t));
}

void server_prompt_disk::scan() {
    std::lock_guard<std::mutex> lock(mutex);
    index.clear();
    index_bytes = 0;

    std::error_code ec;
    for (const auto & de : fs::directory_iterator(dir, ec)) {
        if (ec || !de.is_regular_file()) {
            continue;
        }
        const auto path = de.path().string();
        if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".tmp") == 0) {
            // leftover from a crashed write: never counted in the budget, remove it
            std::error_code ec_tmp;
            fs::remove(path, ec_tmp);
            continue;
        }
        if (path.size() < 7 || path.compare(path.size() - 7, 7, ".centry") != 0) {
            continue;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            continue;
        }
        server_prompt_disk_guard g;
        int32_t n_ctx = 0;
        uint32_t version = 0;
        llama_tokens tokens;
        if (!read_entry_head(f, g, n_ctx, tokens, version)) {
            SRV_WRN("prompt disk: ignoring unreadable entry %s\n", path.c_str());
            continue;
        }
        if (!guard_same(g, guard, version)) {
            SRV_WRN("prompt disk: ignoring entry from another config %s\n", path.c_str());
            continue;
        }
        if ((int64_t) tokens.size() > guard.n_ctx) {
            continue;
        }
        // parse checkpoint positions only (seek past blobs, no data read)
        // then peek the entry kind + full-state presence without loading blobs.
        std::vector<int64_t> ckpt_ntok;
        std::vector<int32_t> ckpt_max;
        bool is_prefix = false;
        bool has_full_state = false;
        {
            uint64_t nckpt = 0;
            if (!rd(f, &nckpt, sizeof(nckpt)) || nckpt > 4096) {
                SRV_WRN("prompt disk: ignoring unreadable entry %s\n", path.c_str());
                continue;
            }
            bool ck_ok = true;
            for (uint64_t i = 0; i < nckpt; i++) {
                int64_t nt = 0;
                int it = 0;
                int32_t p0 = 0, p1 = 0;
                ck_ok = rd(f, &nt, sizeof(nt)) && rd(f, &it, sizeof(it))
                     && rd(f, &p0, sizeof(p0)) && rd(f, &p1, sizeof(p1));
                if (!ck_ok) {
                    break;
                }
                ckpt_ntok.push_back(nt);
                ckpt_max.push_back(p1);
                for (int k = 0; k < 3; k++) {
                    uint64_t bl = 0;
                    ck_ok = rd(f, &bl, sizeof(bl));
                    if (!ck_ok) {
                        break;
                    }
                    f.seekg(bl, std::ios::cur);
                    ck_ok = (bool) f;
                    if (!ck_ok) {
                        break;
                    }
                }
                if (!ck_ok) {
                    break;
                }
            }
            if (!ck_ok) {
                SRV_WRN("prompt disk: ignoring unreadable entry %s\n", path.c_str());
                continue;
            }
            if (version >= 3) {
                uint8_t prefix_flag = 0;
                if (!rd(f, &prefix_flag, sizeof(prefix_flag))) {
                    SRV_WRN("prompt disk: ignoring unreadable entry %s\n", path.c_str());
                    continue;
                }
                is_prefix = prefix_flag != 0;
                uint64_t lmain = 0;
                if (!rd(f, &lmain, sizeof(lmain))) {
                    SRV_WRN("prompt disk: ignoring unreadable entry %s\n", path.c_str());
                    continue;
                }
                has_full_state = lmain != 0;
            } else {
                uint64_t lmain = 0;
                if (!rd(f, &lmain, sizeof(lmain))) {
                    SRV_WRN("prompt disk: ignoring unreadable entry %s\n", path.c_str());
                    continue;
                }
                has_full_state = lmain != 0;
                is_prefix = !has_full_state;
            }
        }
        const uint64_t h = server_prompt_disk_hash(tokens);
        index_entry e;
        e.hash = h;
        e.path = path;
        e.tokens = std::move(tokens);
        e.ckpt_ntok = std::move(ckpt_ntok);
        e.ckpt_max = std::move(ckpt_max);
        e.is_prefix = is_prefix;
        e.has_full_state = has_full_state;
        e.format_version = version;
        e.bytes = 0;
        index.push_back(std::move(e));
        std::error_code ec2;
        index.back().bytes = fs::file_size(path, ec2);
        std::error_code ec3;
        index.back().mtime = fs::last_write_time(path, ec3);
        index_bytes += index.back().bytes;
    }

    SRV_INF("prompt disk: scanned %s, %zu entries, %.3f MiB\n",
            dir.c_str(), index.size(), index_bytes / (1024.0 * 1024.0));
}

void server_prompt_disk::store(
        const server_tokens & tokens,
        const std::list<common_prompt_checkpoint> & checkpoints,
        std::vector<uint8_t> data_main,
        std::vector<uint8_t> data_drft) const {
    if (tokens.size() < min_tokens || checkpoints.empty()) {
        return;
    }
    queue_item item;
    item.tokens = tokens.get_text_tokens();
    item.checkpoints = checkpoints;
    item.data_main = std::move(data_main);
    item.data_drft = std::move(data_drft);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.size() > 4) {
            // writer is behind, drop the oldest pending write (a newer snapshot covers it)
            queue.pop_front();
        }
        queue.push_back(std::move(item));
    }
    cv.notify_one();
}

void server_prompt_disk::store_prefix_checkpoint(
        const server_tokens & tokens,
        const common_prompt_checkpoint & checkpoint,
        size_t prefix_limit,
        std::vector<uint8_t> data_main,
        std::vector<uint8_t> data_drft) const {
    if (tokens.has_mtmd || checkpoint.n_tokens < (int64_t) min_tokens
            || checkpoint.n_tokens > (int64_t) prefix_limit) {
        return;
    }
    // Full-state prefix entry: the KV snapshot must be complete (FLAGS_NONE),
    // otherwise a later session cannot restore from this prefix alone.
    if (data_main.empty()) {
        return;
    }

    queue_item item;
    item.tokens = tokens.get_text_tokens();
    if (checkpoint.n_tokens > (int64_t) item.tokens.size()) {
        return;
    }
    // Save the token prefix up to this checkpoint so a new session can match
    // by longest common prefix and recompute only tokens after the fork.
    item.tokens.resize((size_t) checkpoint.n_tokens);
    // The on-disk prefix entry is restored from data_main/data_drft with
    // LLAMA_STATE_SEQ_FLAGS_NONE.  Do not also serialize the in-RAM
    // PARTIAL_ONLY payload: it is neither needed for prefix restore nor a
    // substitute for the complete snapshot, and would nearly duplicate the
    // cache size.
    common_prompt_checkpoint prefix_checkpoint = checkpoint;
    prefix_checkpoint.data_tgt.clear();
    prefix_checkpoint.data_dft.clear();
    prefix_checkpoint.data_spec.clear();
    item.checkpoints.push_back(std::move(prefix_checkpoint));
    item.data_main = std::move(data_main);
    item.data_drft = std::move(data_drft);
    item.checkpoint_only = true;

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return stop || queue.size() < 2; });
        if (stop) {
            return;
        }
        queue.push_back(std::move(item));
    }
    cv.notify_one();
}

static int common_prefix_len(const llama_tokens & a, const server_tokens & b) {
    int n = 0;
    const size_t nmax = std::min(a.size(), b.size());
    while ((size_t) n < nmax && a[n] == b[n]) {
        n++;
    }
    return n;
}

bool server_prompt_disk::find_best(const server_tokens & query, std::string & out_path, int & out_lcp) const {
    std::lock_guard<std::mutex> lock(mutex);
    out_lcp = 0;
    bool found = false;
    uint32_t best_version = 0;
    for (const auto & e : index) {
        const int lcp = common_prefix_len(e.tokens, query);
        // Usable length depends on whether this entry carries a complete state.
        // - prefix entry with full state: reusable iff the whole stored prefix
        //   is contained in the query (lcp >= stored length). Usable = stored length.
        // - full entry with full state: reusable up to the largest stored
        //   checkpoint n_tokens at or below lcp (rewind via checkpoints), or the
        //   whole entry when the query extends it.
        // - legacy v1/v2 partial entries (no full state): fall back to the old
        //   pos_max heuristic so old files remain readable.
        int usable = 0;
        if (e.has_full_state) {
            if (e.is_prefix) {
                if (lcp >= (int) e.tokens.size() && !e.tokens.empty()) {
                    usable = (int) e.tokens.size();
                }
            } else {
                if (lcp >= (int) e.tokens.size() && !e.tokens.empty()) {
                    usable = (int) e.tokens.size();
                } else {
                    for (const int64_t nt : e.ckpt_ntok) {
                        if (nt <= lcp && nt > usable) {
                            usable = (int) nt;
                        }
                    }
                }
            }
        } else {
            for (const int32_t pmax : e.ckpt_max) {
                if (pmax <= lcp && pmax > usable) {
                    usable = pmax;
                }
            }
        }
        if (usable > out_lcp || (usable == out_lcp && usable > 0 && e.format_version > best_version)) {
            out_lcp = usable;
            out_path = e.path;
            found = true;
            best_version = e.format_version;
        }
    }
    return found;
}

bool server_prompt_disk::load_entry(
        const std::string & path,
        server_prompt & prompt_out,
        server_prompt_data & data_out,
        double & ms_read,
        double & ms_parse) const {
    ms_read = 0.0;
    ms_parse = 0.0;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    // large stream buffer: header/checkpoint small reads stay cheap,
    // blob reads stay sequential. blobs are read directly into their
    // destination vectors (no whole-file staging copy).
    std::vector<char> io_buf(4 * 1024 * 1024);
    f.rdbuf()->pubsetbuf(io_buf.data(), io_buf.size());

    const auto t_parse0 = std::chrono::steady_clock::now();
    server_prompt_disk_guard g;
    int32_t n_ctx = 0;
    uint32_t version = 0;
    llama_tokens tokens;
    if (!read_entry_head(f, g, n_ctx, tokens, version)) {
        return false;
    }
    if (!guard_same(g, guard, version) || (int64_t) tokens.size() > guard.n_ctx) {
        SRV_WRN("prompt disk: refusing entry %s (config changed)\n", path.c_str());
        return false;
    }
    uint64_t nckpt = 0;
    if (!rd(f, &nckpt, sizeof(nckpt)) || nckpt > 4096) {
        return false;
    }
    std::list<common_prompt_checkpoint> ckpts;
    for (uint64_t i = 0; i < nckpt; i++) {
        common_prompt_checkpoint ckpt;
        if (!rd(f, &ckpt.n_tokens, sizeof(ckpt.n_tokens))
                || !rd(f, &ckpt.id_task, sizeof(ckpt.id_task))
                || !rd(f, &ckpt.pos_min, sizeof(ckpt.pos_min))
                || !rd(f, &ckpt.pos_max, sizeof(ckpt.pos_max))) {
            return false;
        }
        for (auto * b : {&ckpt.data_tgt, &ckpt.data_dft, &ckpt.data_spec}) {
            uint64_t bl = 0;
            if (!rd(f, &bl, sizeof(bl)) || bl > (uint64_t) 32 * 1024 * 1024 * 1024) {
                return false;
            }
            if (bl) {
                b->resize((size_t) bl);
                if (!f.read((char *) b->data(), bl)) {
                    return false;
                }
            }
        }
        ckpts.push_back(std::move(ckpt));
    }
    uint64_t lmain = 0, ldrft = 0;
    bool is_prefix = false;
    if (version >= 3) {
        uint8_t prefix_flag = 0;
        if (!rd(f, &prefix_flag, sizeof(prefix_flag))) {
            return false;
        }
        is_prefix = prefix_flag != 0;
    }
    if (!rd(f, &lmain, sizeof(lmain)) || lmain > (uint64_t) 64 * 1024 * 1024 * 1024) {
        return false;
    }
    ms_parse = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_parse0).count();
    // file order is: lmain | main blob | ldrft | drft blob.
    // read each blob straight into its destination vector (zero-copy).
    const auto t_read0 = std::chrono::steady_clock::now();
    data_out.main.resize((size_t) lmain);
    if (lmain && !f.read((char *) data_out.main.data(), lmain)) {
        return false;
    }
    if (!rd(f, &ldrft, sizeof(ldrft)) || ldrft > (uint64_t) 64 * 1024 * 1024 * 1024) {
        return false;
    }
    if (version >= 3) {
        data_out.checkpoint_only = is_prefix;
    } else {
        data_out.checkpoint_only = lmain == 0;
    }
    if (data_out.checkpoint_only && ckpts.empty()) {
        return false;
    }
    if (data_out.checkpoint_only && data_out.main.empty()) {
        SRV_WRN("prompt disk: legacy partial-only entry %s has no full state\n", path.c_str());
    }
    data_out.drft.resize((size_t) ldrft);
    if (ldrft && !f.read((char *) data_out.drft.data(), ldrft)) {
        return false;
    }
    ms_read = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_read0).count();

    // rebuild server_tokens from raw ids
    prompt_out.clear();
    for (const int32_t t : tokens) {
        prompt_out.tokens.push_back(t);
    }
    prompt_out.checkpoints = std::move(ckpts);

    // touch for LRU (file + index row)
    {
        std::error_code ec;
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(path, now, ec);
        std::lock_guard<std::mutex> lock(mutex);
        for (auto & e : index) {
            if (e.path == path) {
                e.mtime = now;
                break;
            }
        }
    }

    SRV_INF("prompt disk: loaded entry %s (%d tokens)\n", path.c_str(), (int) tokens.size());
    return true;
}

size_t server_prompt_disk::entry_count() const {
    std::lock_guard<std::mutex> lock(mutex);
    return index.size();
}

void server_prompt_disk::enforce_budget() const {
    if (budget_bytes == 0) {
        return;
    }
    // all our files are tracked in the in-memory index: no directory rescan.
    // mtimes are recorded at scan/write/load-touch time.
    std::lock_guard<std::mutex> lock(mutex);
    // drop rows whose files vanished out of band
    for (auto it = index.begin(); it != index.end();) {
        std::error_code ec;
        if (fs::exists(it->path, ec)) {
            ++it;
        } else {
            index_bytes -= it->bytes;
            it = index.erase(it);
        }
    }
    for (int guard_iter = 0; guard_iter < 100000; guard_iter++) {
        if (index_bytes <= budget_bytes) {
            break;
        }
        auto oldest = index.end();
        for (auto it = index.begin(); it != index.end(); ++it) {
            if (oldest == index.end() || it->mtime < oldest->mtime) {
                oldest = it;
            }
        }
        if (oldest == index.end()) {
            break;
        }
        std::error_code ec4;
        fs::remove(oldest->path, ec4);
        SRV_WRN("prompt disk: budget exceeded, evicted %s\n", oldest->path.c_str());
        index_bytes -= oldest->bytes;
        index.erase(oldest);
    }
}

void server_prompt_disk::write_entry(const queue_item & item) const {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t h = server_prompt_disk_hash(item.tokens);
    const std::string path = path_for(h);

    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto & e : index) {
            if (e.hash == h && e.path == path) {
                // exact duplicate, refresh LRU time and skip
                std::error_code ec;
                fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
                return;
            }
        }
    }

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            SRV_WRN("prompt disk: cannot write %s\n", tmp.c_str());
            return;
        }
        // large buffer: header small writes stay cheap, GB blob writes go out in big chunks
        std::vector<char> io_buf(4 * 1024 * 1024);
        f.rdbuf()->pubsetbuf(io_buf.data(), io_buf.size());
        const uint32_t magic = PDC_MAGIC, ver = PDC_VER;
        wr(f, &magic, sizeof(magic));
        wr(f, &ver, sizeof(ver));
        wr_str(f, guard.model_fingerprint);
        wr(f, &guard.n_ctx, sizeof(guard.n_ctx));
        wr(f, &guard.flash_attn, sizeof(guard.flash_attn));
        wr(f, &guard.cache_k, sizeof(guard.cache_k));
        wr(f, &guard.cache_v, sizeof(guard.cache_v));
        wr_str(f, guard.build);
        const uint64_t ntok = item.tokens.size();
        wr(f, &ntok, sizeof(ntok));
        if (ntok) {
            f.write((const char *) item.tokens.data(), ntok * sizeof(int32_t));
        }
        const uint64_t nckpt = item.checkpoints.size();
        wr(f, &nckpt, sizeof(nckpt));
        for (const auto & ckpt : item.checkpoints) {
            wr(f, &ckpt.n_tokens, sizeof(ckpt.n_tokens));
            wr(f, &ckpt.id_task, sizeof(ckpt.id_task));
            wr(f, &ckpt.pos_min, sizeof(ckpt.pos_min));
            wr(f, &ckpt.pos_max, sizeof(ckpt.pos_max));
            for (const auto * b : {&ckpt.data_tgt, &ckpt.data_dft, &ckpt.data_spec}) {
                const uint64_t bl = b->size();
                wr(f, &bl, sizeof(bl));
                if (bl) {
                    f.write((const char *) b->data(), bl);
                }
            }
        }
        // v3: explicit prefix flag so prefix entries with full state are not
        // mistaken for full-prompt entries. v1/v2 inferred prefix from lmain==0.
        const uint8_t prefix_flag = item.checkpoint_only ? 1 : 0;
        wr(f, &prefix_flag, sizeof(prefix_flag));
        const uint64_t lmain = item.data_main.size(), ldrft = item.data_drft.size();
        wr(f, &lmain, sizeof(lmain));
        if (lmain) {
            f.write((const char *) item.data_main.data(), lmain);
        }
        wr(f, &ldrft, sizeof(ldrft));
        if (ldrft) {
            f.write((const char *) item.data_drft.data(), ldrft);
        }
        f.close();
        if (!f) {
            SRV_WRN("prompt disk: failed writing %s\n", tmp.c_str());
            std::error_code ec;
            fs::remove(tmp, ec);
            return;
        }
        // flush OS buffers before rename so a crash cannot leave a torn entry
#ifdef _WIN32
        {
            HANDLE h = CreateFileA(tmp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(h);
                CloseHandle(h);
            }
        }
#else
        {
            const int fd = ::open(tmp.c_str(), O_RDONLY);
            if (fd >= 0) {
                ::fsync(fd);
                ::close(fd);
            }
        }
#endif
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return;
    }

    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::error_code ec2;
        bytes = fs::file_size(path, ec2);
        index_entry e;
        e.hash = h;
        e.path = path;
        e.tokens = item.tokens;
        for (const auto & ckpt : item.checkpoints) {
            e.ckpt_ntok.push_back(ckpt.n_tokens);
            e.ckpt_max.push_back(ckpt.pos_max);
        }
        e.is_prefix = item.checkpoint_only;
        e.has_full_state = !item.data_main.empty();
        e.format_version = PDC_VER;
        e.bytes = bytes;
        e.mtime = fs::file_time_type::clock::now();
        index.push_back(std::move(e));
        index_bytes += bytes;

        if (!item.checkpoint_only) {
            // A full prompt entry covers shorter entries with the same prefix.
            for (auto it = index.begin(); it != index.end();) {
                const bool exact_legacy = it->hash == h && it->tokens == item.tokens && it->path != path;
                const bool shorter_prefix = it->hash != h
                        && it->tokens.size() < item.tokens.size()
                        && std::equal(it->tokens.begin(), it->tokens.end(), item.tokens.begin());
                if (exact_legacy || shorter_prefix) {
                    std::error_code ec5;
                    fs::remove(it->path, ec5);
                    SRV_INF("prompt disk: pruned superseded entry %s (%d tokens)\n",
                            it->path.c_str(), (int) it->tokens.size());
                    index_bytes -= it->bytes;
                    it = index.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    enforce_budget();

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (item.checkpoint_only) {
        SRV_INF("prompt disk: stored prefix entry %s (%d tokens, %.1f MiB, %.0f ms, full state)\n",
                path.c_str(), (int) item.tokens.size(), bytes / (1024.0 * 1024.0), ms);
    } else {
        SRV_INF("prompt disk: stored entry %s (%d tokens, %.1f MiB, %.0f ms)\n",
                path.c_str(), (int) item.tokens.size(), bytes / (1024.0 * 1024.0), ms);
    }
}

void server_prompt_disk::worker_main() const {
    while (true) {
        queue_item item;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this]() { return stop || !queue.empty(); });
            if (stop && queue.empty()) {
                return;
            }
            // coalesce: a newer queued snapshot whose tokens extend an older one covers it
            while (queue.size() > 1 && !queue.front().checkpoint_only && !queue.back().checkpoint_only) {
                const auto & a = queue.front().tokens;
                const auto & b = queue.back().tokens;
                if (a.size() < b.size()) {
                    bool prefix = true;
                    for (size_t i = 0; i < a.size(); i++) {
                        if (a[i] != b[i]) {
                            prefix = false;
                            break;
                        }
                    }
                    if (prefix) {
                        queue.pop_front();
                        continue;
                    }
                }
                break;
            }
            item = std::move(queue.front());
            queue.pop_front();
        }
        cv.notify_all();
        try {
            write_entry(item);
        } catch (const std::exception & e) {
            SRV_WRN("prompt disk: write failed: %s\n", e.what());
        }
    }
}
