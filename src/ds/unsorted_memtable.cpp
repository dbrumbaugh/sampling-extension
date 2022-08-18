/*
 *
 */

#include "ds/unsorted_memtable.hpp"

namespace lsm { namespace ds {

UnsortedMemTable::UnsortedMemTable(size_t capacity, global::g_state *state, bool rejection_sampling) : data_array(nullptr, free)
{
    this->buffer_size = capacity * state->record_schema->record_length();
    this->data_array = mem::create_aligned_buffer(buffer_size);

    this->table = std::vector<io::Record>(capacity, io::Record());

    this->state = state;
    this->current_tail = 0;
    this->tombstones = 0;

    this->record_cap = capacity;

    this->key_cmp = this->state->record_schema->get_key_cmp();
    this->tombstone_cache = std::make_unique<util::TombstoneCache>(-1, state->record_schema.get(), true);

    this->thread_pins = 0;

    this->rejection_sampling = rejection_sampling;
}


UnsortedMemTable::~UnsortedMemTable()
{
    this->truncate();
}


int UnsortedMemTable::insert(byte *key, byte *value, Timestamp time, bool tombstone)
{
    auto idx = this->get_index();

    // there is no room left for the insert
    if (idx == -1) {
        return 0;
    }

    auto rec_ptr = this->data_array.get() + (idx * this->state->record_schema->record_length());
    this->state->record_schema->create_record_at(rec_ptr, key, value);

    auto record = io::Record(rec_ptr, this->state->record_schema->record_length(), time, tombstone);

    if (record.is_tombstone()) {
        tombstones.fetch_add(1);
        tombstone_cache->insert(key, value, time);
    }

    this->finalize_insertion(idx, record);

    return 1;
}


int UnsortedMemTable::remove(byte * /*key*/, byte * /*value*/, Timestamp /*time*/)
{
    return 0;
}


io::Record UnsortedMemTable::get(const byte *key, Timestamp time) 
{
    auto idx = this->find_record(key, time);

    if (idx == -1) {
        return io::Record();
    }

    return this->table[idx];
}


io::Record UnsortedMemTable::get(size_t idx) 
{
    if (idx >= this->table.size()){
        return io::Record();
    }

    return this->table[idx];
}


size_t UnsortedMemTable::get_record_count()
{
    return std::min((size_t) this->current_tail, this->table.size());
}


size_t UnsortedMemTable::get_capacity()
{
    return this->table.size();
}


bool UnsortedMemTable::is_full()
{
    return this->get_record_count() == this->get_capacity();
}


bool UnsortedMemTable::has_tombstone(const byte *key, const byte *val, Timestamp time)
{
    return this->tombstone_cache->exists(key, val, time);
}


bool UnsortedMemTable::truncate()
{
    if (this->thread_pins > 0) {
        return false;
    }

    // We need to re-zero the record vector to ensure that sampling during the
    // gap between an insert obtaining an index, and finalizing the insert,
    // doesn't return an old record. This ensures that, should this occur, the
    // resulting record will be tagged as invalid and skipped until insert
    // finalization.
    this->table = std::vector<io::Record>(this->record_cap, io::Record());
    
    this->current_tail = 0;
    this->tombstones = 0;

    this->tombstone_cache->truncate();

    return true;
}


std::unique_ptr<sampling::SampleRange> UnsortedMemTable::get_sample_range(byte *lower_key, byte *upper_key)
{
    this->thread_pin();
    if (this->rejection_sampling) {
        return std::make_unique<sampling::UnsortedRejectionSampleRange>(this->get_record_count() - 1, lower_key, upper_key, this->state, this);
    } 
    return std::make_unique<sampling::UnsortedMemTableSampleRange>(this->table.begin(), this->table.begin() + this->get_record_count(), lower_key, upper_key, this->state, this);
}


std::unique_ptr<iter::GenericIterator<io::Record>> UnsortedMemTable::start_sorted_scan()
{
    this->thread_pin();
    return std::make_unique<UnsortedRecordIterator>(this, this->state);
}


ssize_t UnsortedMemTable::find_record(const byte *key, Timestamp time)
{
    ssize_t current_best_match = -1;
    Timestamp current_best_time = 0;

    ssize_t upper_bound = this->get_record_count() - 1;
    for (size_t i=0; i<=(size_t)upper_bound; i++) {
        if (this->table[i].get_timestamp() <= time) {
            const byte *table_key = this->state->record_schema->get_key(this->table[i].get_data()).Bytes();
            if (this->key_cmp(key, table_key) == 0) {
                if (this->table[i].get_timestamp() >= current_best_time) {
                    current_best_time = this->table[i].get_timestamp();
                    current_best_match = i;
                }
            }
        }
    }

    return current_best_match;
}


size_t UnsortedMemTable::tombstone_count() 
{
    return this->tombstones;
}


ssize_t UnsortedMemTable::get_index()
{
    size_t idx = this->current_tail.fetch_add(1);    

    // there is space, so return the reserved index
    if (idx < this->table.size()) {
        return idx;
    }

    // no space in the buffer
    return -1;
}


void UnsortedMemTable::finalize_insertion(size_t idx, io::Record record)
{
    this->table[idx] = record;
}


UnsortedRecordIterator::UnsortedRecordIterator(UnsortedMemTable *table, global::g_state *state)
{
    this->cmp.rec_cmp = state->record_schema->get_record_cmp();
    
    // Copy the records into the iterator and sort them
    this->sorted_records = table->table;
    std::sort(sorted_records.begin(), sorted_records.end(), this->cmp);

    this->table = table;
    this->unpinned = false;

    this->current_index = -1;
}


bool UnsortedRecordIterator::next()
{
    while ((size_t) ++this->current_index < this->element_count() && this->get_item().is_valid()) {
        return true;
    }

    return false;
}


io::Record UnsortedRecordIterator::get_item()
{
    return this->sorted_records[this->current_index];
}


void UnsortedRecordIterator::end_scan()
{
    this->table->thread_unpin();
    this->unpinned = true;
}

UnsortedRecordIterator::~UnsortedRecordIterator()
{
    if (!this->unpinned) {
        this->table->thread_unpin();
    }
}

}}
