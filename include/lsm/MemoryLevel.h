#pragma once

#include <vector>

#include "util/types.h"
#include "lsm/InMemRun.h"
#include "ds/BloomFilter.h"

namespace lsm {

class MemoryLevel {
public:
    MemoryLevel(ssize_t level_no, size_t run_cap)
    : m_level_no(level_no), m_run_cap(run_cap), m_run_cnt(0)
    , m_runs(new InMemRun*[run_cap]{nullptr})
    , m_bfs(new BloomFilter*[run_cap]{nullptr}) {}

    ~MemoryLevel() {
        for (size_t i = 0; i < m_run_cap; ++i) {
            if (m_runs[i]) delete m_runs[i];
            if (m_bfs[i]) delete m_bfs[i];
        }

        delete[] m_runs;
        delete[] m_bfs;
    }

    void append_run(InMemRun* run) {
        assert(m_run_cnt < m_run_cap);
        m_runs[m_run_cnt ++] = run;
    }

    // Append the sample range in-order.....
    void get_sample_ranges(std::vector<SampleRange>& dst, const char* low, const char* high) {
        for (ssize_t i = 0; i < m_run_cnt; ++i) {
            dst.emplace_back(SampleRange{RunId{m_level_no, i}, m_runs[i]->get_lower_bound(low), m_runs[i]->get_upper_bound(high)});
        }
    }

    bool bf_rejection_check(size_t run_stop, const char* key) {
        for (size_t i = 0; i < run_stop; ++i) {
            if (m_bfs[i] && m_bfs[i]->lookup(key, key_size))
                return true;
        }
        return false;
    }

    bool tombstone_check(size_t run_stop, const char* key, const char* val) {
        for (size_t i = 0; i < run_stop;  ++i) {
            if (m_runs[i] && m_runs[i]->check_tombstone(key, val))
                return true;
        }
        return false;
    }

    const char* get_record_at(size_t run_no, size_t idx) {
        return m_runs[run_no]->get_record_at(idx);
    }
    
    InMemRun* get_run(size_t idx) {
        return m_runs[idx];
    }

    size_t get_run_count() {
        return m_run_cnt;
    }

private:
    ssize_t m_level_no;
    size_t m_run_cap;
    size_t m_run_cnt;
    InMemRun** m_runs;
    BloomFilter** m_bfs;

};

}
