#pragma once

#include <cstdlib>
#include <atomic>
#include <cassert>
#include <vector>

#include "util/base.h"
#include "util/bf_config.h"
#include "ds/BloomFilter.h"
#include "util/record.h"

namespace lsm {

class MemTable {
public:
    MemTable(size_t capacity, bool rej_sampling, size_t max_tombstone_cap, const gsl_rng* rng)
    : m_cap(capacity), m_tombstone_cap(max_tombstone_cap), m_buffersize(capacity * record_size), m_reccnt(0)
    , m_tombstonecnt(0), m_current_tail(0) {
        m_data = (char*) std::aligned_alloc(CACHELINE_SIZE, m_buffersize);
        m_tombstone_filter = nullptr;
        if (max_tombstone_cap > 0) {
            assert(rng != nullptr);
            m_tombstone_filter = new BloomFilter(BF_FPR, max_tombstone_cap, BF_HASH_FUNCS, rng);
        }
    }

    ~MemTable() {
        if (m_data) free(m_data);
        if (m_tombstone_filter) delete m_tombstone_filter;
    }

    int append(const char* key, const char* value, bool is_tombstone = false) {
        if (is_tombstone && m_tombstonecnt + 1 > m_tombstone_cap) return 0;

        ssize_t pos = 0;
        if ((pos = try_advance_tail()) == -1) return 0;


        layout_memtable_record(m_data + pos, key, value, is_tombstone, (uint32_t)pos / record_size);
        if (is_tombstone) {
            m_tombstonecnt.fetch_add(1);
            if (m_tombstone_filter) m_tombstone_filter->insert(key, key_size);
        }
        m_reccnt.fetch_add(1);

        return 1;     
    }

    bool truncate() {
        m_current_tail.store(0);
        m_tombstonecnt.store(0);
        m_reccnt.store(0);
        if (m_tombstone_filter) m_tombstone_filter->clear();

        return true;
    }

    char* sorted_output() {
        qsort(m_data, m_reccnt.load(), record_size, memtable_record_cmp);
        return m_data;
    }
    
    size_t get_record_count() {
        return m_reccnt;
    }
    
    size_t get_capacity() {
        return m_cap;
    }

    bool is_full() {
        return m_reccnt == m_cap;
    }

    size_t get_tombstone_count() {
        return m_tombstonecnt.load();
    }

    bool check_tombstone(const char* key, const char* value) {
        if (m_tombstone_filter && !m_tombstone_filter->lookup(key, key_size)) return false;

        auto offset = 0;
        while (offset < m_current_tail) {
            if (record_match(m_data + offset, key, value, true)) return true;
            offset += record_size;
        }
        return false;
    }

    void create_sampling_vector(const char *min, const char *max, std::vector<const char *> &records) {
        records.clear();
        for (size_t i=0; i<m_reccnt.load(); i++) {
            auto rec = this->get_record_at(i);
            auto key = get_key(rec);
            if (key_cmp(key, min) >= 0 && key_cmp(key, max) <= 0) {
                records.push_back(rec);
            }
        }
    }

    const char *get_record_at(size_t idx) {
        return m_data + (record_size * idx);
    }

    size_t get_memory_utilization() {
        return m_buffersize;
    }

    size_t get_aux_memory_utilization() {
        return m_tombstone_filter->get_memory_utilization();
    }

    size_t get_tombstone_capacity() {
        return m_tombstone_cap;
    }

private:
    ssize_t try_advance_tail() {
        size_t new_tail = m_current_tail.fetch_add(record_size);

        if (new_tail < m_buffersize) return new_tail;
        else return -1;
    }

    size_t m_cap;
    size_t m_buffersize;
    size_t m_tombstone_cap;
    
    char* m_data;
    BloomFilter* m_tombstone_filter;

    alignas(64) std::atomic<size_t> m_tombstonecnt;
    alignas(64) std::atomic<size_t> m_current_tail;
    alignas(64) std::atomic<size_t> m_reccnt;
};

}
