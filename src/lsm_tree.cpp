#include <cassert>
#include <fstream>
#include <iostream>
#include <map>

#include "lsm_tree.h"
#include "merge.h"
#include "sys.h"

using namespace std;

ostream& operator<<(ostream& stream, const entry_t& entry) {
    stream.write((char *)&entry.key, sizeof(KEY_t));
    stream.write((char *)&entry.val, sizeof(VAL_t));
    return stream;
}

istream& operator>>(istream& stream, entry_t& entry) {
    stream.read((char *)&entry.key, sizeof(KEY_t));
    stream.read((char *)&entry.val, sizeof(VAL_t));
    return stream;
}

/*
 * LSM Tree
 */

LSMTree::LSMTree(int buffer_max_entries, int depth, int fanout, int num_threads, float merge_ratio)
    : buffer(buffer_max_entries), worker_pool(num_threads), merge_ratio(merge_ratio)
{
    long max_run_size;

    max_run_size = buffer_max_entries;

    while ((depth--) > 0) {
        levels.emplace_back(fanout, max_run_size);
        max_run_size *= fanout;
    }
}

void LSMTree::merge_down(vector<Level>::iterator current) {
    vector<Level>::iterator next;
    MergeContext merge_ctx;
    entry_t entry;
    int merge_size, i;

    assert(current >= levels.begin());

    if (current->runs.empty()) {
        return;
    } else if (current >= levels.end() - 1) {
        die("No more space in tree.");
    } else {
        next = current + 1;
    }

    /*
     * If the next level does not have space for the current level,
     * recursively merge the next level downwards to create some
     */

    if (next->remaining() == 0) {
        merge_down(next);
        assert(next->remaining() > 0);
    }

    /*
     * Add the first "merge_size" runs in the current level
     * to the merge context
     */

    merge_size = merge_ratio * current->max_runs;
    if (merge_size > current->runs.size()) merge_size = current->runs.size();

    for (i = 0; i < merge_size; i++) {
        merge_ctx.add(current->runs[i].map_read(), current->runs[i].size);
    }

    /*
     * Merge the context into the last run in the next level
     */

    next->runs.emplace_back(next->max_run_size);
    next->runs.back().map_write();

    while (!merge_ctx.done()) {
        entry = merge_ctx.next();

        // Remove deleted keys from the final level
        if (!(next == levels.end() - 1 && entry.val == VAL_TOMBSTONE)) {
            next->runs.back().put(entry);
        }
    }

    next->runs.back().unmap_write();

    /*
     * Unmap and delete the old (now redundant) entry files.
     */

    for (i = 0; i < merge_size; i++) {
        current->runs[i].unmap_read();
    }

    current->runs.erase(current->runs.begin(), current->runs.begin() + merge_size);
}

void LSMTree::put(KEY_t key, VAL_t val) {
    /*
     * Try inserting the key into the buffer
     */

    if (buffer.put(key, val)) {
        return;
    }

    /*
     * If the buffer is full, then flush level 0 if necessary
     */

    if (levels.front().remaining() == 0) {
        merge_down(levels.begin());
    }

    /*
     * Flush the buffer to level 0
     */

    levels.front().runs.emplace_back(levels.front().max_run_size);
    levels.front().runs.back().map_write();

    for (const auto& entry : buffer.entries) {
        levels.front().runs.back().put(entry);
    }

    levels.front().runs.back().unmap_write();

    /*
     * Empty the buffer and insert the key/value pair
     */

    buffer.empty();
    assert(buffer.put(key, val));
}

Run * LSMTree::get_run(int index) {
    for (const auto& level : levels) {
        if (index < level.runs.size()) {
            // The latest runs are at the back
            return (Run *) &level.runs[level.runs.size() - index - 1];
        } else {
            index -= level.runs.size();
        }
    }

    return nullptr;
};

void LSMTree::get(KEY_t key) {
    VAL_t *buffer_val;
    VAL_t latest_val;
    int latest_run;
    SpinLock lock;
    atomic<int> counter;

    /*
     * Search buffer
     */

    buffer_val = buffer.get(key);

    if (buffer_val != nullptr) {
        if (*buffer_val != VAL_TOMBSTONE) cout << *buffer_val;
        cout << endl;
        delete buffer_val;
        return;
    }

    /*
     * Search runs
     */

    counter = 0;
    latest_run = -1;

    worker_task search = [&] {
        int current_run;
        Run *run;
        VAL_t *current_val;

        current_run = counter++;

        if (latest_run >= 0 || (run = get_run(current_run)) == nullptr) {
            // Stop search if we discovered a key in another run, or
            // if there are no more runs to search
            return;
        } else if ((current_val = run->get(key)) == nullptr) {
            // Couldn't find the key in the current run, so we need
            // to keep searching.
            search();
        } else {
            // Update val if the run is more recent than the
            // last, then stop searching since there's no need
            // to search later runs.
            lock.lock();

            if (latest_run < 0 || current_run < latest_run) {
                latest_run = current_run;
                latest_val = *current_val;
            }

            lock.unlock();
            delete current_val;
        }
    };

    worker_pool.launch(search);
    worker_pool.wait_all();

    if (latest_run >= 0 && latest_val != VAL_TOMBSTONE) cout << latest_val;
    cout << endl;
}

void LSMTree::range(KEY_t start, KEY_t end) {
    vector<entry_t> *buffer_range;
    map<int, vector<entry_t> *> ranges;
    SpinLock lock;
    atomic<int> counter;
    MergeContext merge_ctx;
    entry_t entry;

    if (end <= start) {
        cout << endl;
        return;
    } else {
        // Convert to inclusive bound
        end -= 1;
    }

    /*
     * Search buffer
     */

    ranges.insert({0, buffer.range(start, end)});

    /*
     * Search runs
     */

    counter = 0;

    worker_task search = [&] {
        int current_run;
        Run *run;

        current_run = counter++;

        if ((run = get_run(current_run)) != nullptr) {
            lock.lock();
            ranges.insert({current_run + 1, run->range(start, end)});
            lock.unlock();

            // Potentially more runs to search.
            search();
        }
    };

    worker_pool.launch(search);
    worker_pool.wait_all();

    /*
     * Merge ranges and print keys
     */

    for (const auto& range : ranges) {
        merge_ctx.add(range.second->data(), range.second->size());
    }

    while (!merge_ctx.done()) {
        entry = merge_ctx.next();
        if (entry.val != VAL_TOMBSTONE) {
            cout << entry.key << ":" << entry.val;
            if (!merge_ctx.done()) cout << " ";
        }
    }

    cout << endl;

    /*
     * Cleanup subrange vectors
     */

    for (auto& range : ranges) {
        delete range.second;
    }
}

void LSMTree::del(KEY_t key) {
    put(key, VAL_TOMBSTONE);
}

void LSMTree::load(string file_path) {
    ifstream stream;
    entry_t entry;

    stream.open(file_path, ifstream::binary);

    if (stream.is_open()) {
        while (stream >> entry) {
            put(entry.key, entry.val);
        }
    } else {
        die("Could not locate file '" + file_path + "'.");
    }
}
