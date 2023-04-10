#pragma once

#include <vector>
#include <cassert>
#include <queue>
#include <memory>

#include "lsm/MemTable.h"
#include "ds/PriorityQueue.h"
#include "util/Cursor.h"

namespace lsm {

constexpr size_t inmem_isam_node_size = 256;

constexpr size_t inmem_isam_fanout = inmem_isam_node_size / (sizeof(key_t) + sizeof(char*));
constexpr size_t inmem_isam_leaf_fanout = inmem_isam_node_size / sizeof(record_t);
constexpr size_t inmem_isam_node_keyskip = sizeof(key_t) * inmem_isam_fanout;

struct InMemISAMNode {
    key_t keys[inmem_isam_fanout];
    char* child[inmem_isam_fanout];
};

static_assert(sizeof(InMemISAMNode) == inmem_isam_node_size, "node size does not match");

thread_local size_t mrun_cancelations = 0;

class InMemRun {
public:
    InMemRun(MemTable* mem_table, BloomFilter* bf)
    :m_reccnt(0), m_tombstone_cnt(0), m_isam_nodes(nullptr), m_deleted_cnt(0) {

        size_t alloc_size = (mem_table->get_record_count() * sizeof(record_t)) + (CACHELINE_SIZE - (mem_table->get_record_count() * sizeof(record_t)) % CACHELINE_SIZE);
        assert(alloc_size % CACHELINE_SIZE == 0);
        m_data = (record_t*)std::aligned_alloc(CACHELINE_SIZE, alloc_size);

        size_t offset = 0;
        m_reccnt = 0;
        record_t* base = mem_table->sorted_output();
        record_t* stop = base + mem_table->get_record_count();
        while (base < stop) {
            if (!base->is_tombstone() && (base + 1 < stop)
                && base->match(base + 1) && (base + 1)->is_tombstone()) {
                base += 2;
                mrun_cancelations++;
                continue;
            } 

            //Masking off the ts.
            base->header &= 1;
            m_data[m_reccnt++] = *base;
            if (bf && base->is_tombstone()) {
                ++m_tombstone_cnt;
                bf->insert(base->key);
            }

            base++;
        }

        if (m_reccnt > 0) {
            build_internal_levels();
        }
    }

    InMemRun(InMemRun** runs, size_t len, BloomFilter* bf)
    :m_reccnt(0), m_tombstone_cnt(0), m_deleted_cnt(0), m_isam_nodes(nullptr) {
        std::vector<Cursor> cursors;
        cursors.reserve(len);

        PriorityQueue pq(len);

        size_t attemp_reccnt = 0;
        
        for (size_t i = 0; i < len; ++i) {
            if (runs[i]) {
                auto base = runs[i]->sorted_output();
                cursors.emplace_back(Cursor{base, base + runs[i]->get_record_count(), 0, runs[i]->get_record_count()});
                attemp_reccnt += runs[i]->get_record_count();
                pq.push(cursors[i].ptr, i);
            } else {
                cursors.emplace_back(Cursor{nullptr, nullptr, 0, 0});
            }
        }

        size_t alloc_size = (attemp_reccnt * sizeof(record_t)) + (CACHELINE_SIZE - (attemp_reccnt * sizeof(record_t)) % CACHELINE_SIZE);
        assert(alloc_size % CACHELINE_SIZE == 0);
        m_data = (record_t*)std::aligned_alloc(CACHELINE_SIZE, alloc_size);

        size_t offset = 0;
        
        while (pq.size()) {
            auto now = pq.peek();
            auto next = pq.size() > 1 ? pq.peek(1) : queue_record{nullptr, 0};
            if (!now.data->is_tombstone() && next.data != nullptr &&
                now.data->match(next.data) && next.data->is_tombstone()) {
                
                pq.pop(); pq.pop();
                auto& cursor1 = cursors[now.version];
                auto& cursor2 = cursors[next.version];
                if (advance_cursor(cursor1)) pq.push(cursor1.ptr, now.version);
                if (advance_cursor(cursor2)) pq.push(cursor2.ptr, next.version);
                continue;
            } 
            auto& cursor = cursors[now.version];
            m_data[m_reccnt++] = *cursor.ptr;
            if (cursor.ptr->is_tombstone()) {
                ++m_tombstone_cnt;
                bf->insert(cursor.ptr->key);
            }
            pq.pop();
            
            if (advance_cursor(cursor)) pq.push(cursor.ptr, now.version);
        }

        if (m_reccnt > 0) {
            build_internal_levels();
        }
    }

    ~InMemRun() {
        if (m_data) free(m_data);
        if (m_isam_nodes) free(m_isam_nodes);
    }

    record_t* sorted_output() const {
        return m_data;
    }
    
    size_t get_record_count() const {
        return m_reccnt;
    }

    size_t get_tombstone_count() const {
        return m_tombstone_cnt;
    }

    bool delete_record(const key_t& key, const value_t& val) {
        size_t idx = get_lower_bound(key);
        if (idx >= m_reccnt) {
            return false;
        }

        while (idx < m_reccnt && m_data[idx].lt(key, val)) ++idx;

        if (m_data[idx].match(key, val, false)) {
            m_data[idx].set_delete_status();
            m_deleted_cnt++;
            return true;
        }

        return false;
    }

    const record_t* get_record_at(size_t idx) const {
        return (idx < m_reccnt) ? m_data + idx : nullptr;
    }

    size_t get_lower_bound(const key_t& key) const {
        const InMemISAMNode* now = m_root;
        while (!is_leaf(reinterpret_cast<const char*>(now))) {
            const InMemISAMNode* next = nullptr;
            for (size_t i = 0; i < inmem_isam_fanout - 1; ++i) {
                if (now->child[i + 1] == nullptr || key <= now->keys[i]) {
                    next = reinterpret_cast<InMemISAMNode*>(now->child[i]);
                    break;
                }
            }

            now = next ? next : reinterpret_cast<const InMemISAMNode*>(now->child[inmem_isam_fanout - 1]);
        }

        const record_t* pos = reinterpret_cast<const record_t*>(now);
        while (pos < m_data + m_reccnt && pos->key < key) pos++;

        return pos - m_data;
    }

    size_t get_upper_bound(const key_t& key) const {
        const InMemISAMNode* now = m_root;
        while (!is_leaf(reinterpret_cast<const char*>(now))) {
            const InMemISAMNode* next = nullptr;
            for (size_t i = 0; i < inmem_isam_fanout - 1; ++i) {
                if (now->child[i + 1] == nullptr || key < now->keys[i]) {
                    next = reinterpret_cast<InMemISAMNode*>(now->child[i]);
                    break;
                }
            }

            now = next ? next : reinterpret_cast<const InMemISAMNode*>(now->child[inmem_isam_fanout - 1]);
        }

        const record_t* pos = reinterpret_cast<const record_t*>(now);
        while (pos < m_data + m_reccnt && pos->key <= key) pos++;

        return pos - m_data;
    }

    bool check_tombstone(const key_t& key, const value_t& val) const {
        size_t idx = get_lower_bound(key);
        if (idx >= m_reccnt) {
            return false;
        }

        record_t* ptr = m_data + idx;

        while (ptr < m_data + m_reccnt && ptr->lt(key, val)) ptr++;
        return ptr->match(key, val, true);
    }

    size_t get_memory_utilization() {
        return m_reccnt * sizeof(record_t) + m_internal_node_cnt * inmem_isam_node_size;
    }

    void persist_to_file(std::string data_fname) {
        FILE *file = fopen(data_fname.c_str(), "wb");
        assert(file);
        fwrite(m_data, sizeof(record_t), m_reccnt, file);
        fclose(file);
    }
    
private:
    void build_internal_levels() {
        size_t n_leaf_nodes = m_reccnt / inmem_isam_leaf_fanout + (m_reccnt % inmem_isam_leaf_fanout != 0);
        size_t level_node_cnt = n_leaf_nodes;
        size_t node_cnt = 0;
        do {
            level_node_cnt = level_node_cnt / inmem_isam_fanout + (level_node_cnt % inmem_isam_fanout != 0);
            node_cnt += level_node_cnt;
        } while (level_node_cnt > 1);

        size_t alloc_size = (node_cnt * inmem_isam_node_size) + (CACHELINE_SIZE - (node_cnt * inmem_isam_node_size) % CACHELINE_SIZE);
        assert(alloc_size % CACHELINE_SIZE == 0);

        m_isam_nodes = (InMemISAMNode*)std::aligned_alloc(CACHELINE_SIZE, alloc_size);
        m_internal_node_cnt = node_cnt;
        memset(m_isam_nodes, 0, node_cnt * inmem_isam_node_size);

        InMemISAMNode* current_node = m_isam_nodes;

        const record_t* leaf_base = m_data;
        const record_t* leaf_stop = m_data + m_reccnt;
        while (leaf_base < leaf_stop) {
            size_t fanout = 0;
            for (size_t i = 0; i < inmem_isam_fanout; ++i) {
                auto rec_ptr = leaf_base + inmem_isam_leaf_fanout * i;
                if (rec_ptr >= leaf_stop) break;
                const record_t* sep_key = std::min(rec_ptr + inmem_isam_leaf_fanout - 1, leaf_stop - 1);
                current_node->keys[i] = sep_key->key;
                current_node->child[i] = (char*)rec_ptr;
                ++fanout;
            }
            current_node++;
            leaf_base += fanout * inmem_isam_leaf_fanout;
        }

        auto level_start = m_isam_nodes;
        auto level_stop = current_node;
        auto current_level_node_cnt = level_stop - level_start;
        while (current_level_node_cnt > 1) {
            auto now = level_start;
            while (now < level_stop) {
                size_t child_cnt = 0;
                for (size_t i = 0; i < inmem_isam_fanout; ++i) {
                    auto node_ptr = now + i;
                    ++child_cnt;
                    if (node_ptr >= level_stop) break;
                    current_node->keys[i] = node_ptr->keys[inmem_isam_fanout - 1];
                    current_node->child[i] = (char*)node_ptr;
                }
                now += child_cnt;
                current_node++;
            }
            level_start = level_stop;
            level_stop = current_node;
            current_level_node_cnt = level_stop - level_start;
        }
        
        assert(current_level_node_cnt == 1);
        m_root = level_start;
    }

    bool is_leaf(const char* ptr) const {
        return ptr >= (const char*)m_data && ptr < (const char*)(m_data + m_reccnt);
    }

    // Members: sorted data, internal ISAM levels, reccnt;
    record_t* m_data;
    InMemISAMNode* m_isam_nodes;
    InMemISAMNode* m_root;
    size_t m_reccnt;
    size_t m_tombstone_cnt;
    size_t m_internal_node_cnt;
    size_t m_deleted_cnt;
};

}
