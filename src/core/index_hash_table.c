#include "moar.h"

#define INDEX_LOAD_FACTOR 0.75
#define INDEX_MIN_SIZE_BASE_2 3

MVM_STATIC_INLINE void hash_demolish_internal(MVMThreadContext *tc,
                                              struct MVMIndexHashTableControl *control) {
    size_t actual_items = MVM_index_hash_kompromat(control);
    size_t entries_size = sizeof(struct MVMIndexHashEntry) * actual_items;
    char *start = (char *)control - entries_size;
    MVM_free(start);
}

/* Frees the entire contents of the hash, leaving you just the hashtable itself,
   which you allocated (heap, stack, inside another struct, wherever) */
void MVM_index_hash_demolish(MVMThreadContext *tc, MVMIndexHashTable *hashtable) {
    struct MVMIndexHashTableControl *control = hashtable->table;
    if (!control)
        return;
    hash_demolish_internal(tc, control);
    hashtable->table = NULL;
}
/* and then free memory if you allocated it */


MVM_STATIC_INLINE struct MVMIndexHashTableControl *hash_allocate_common(MVMThreadContext *tc,
                                                                        MVMuint8 key_right_shift,
                                                                        MVMuint32 official_size) {
    MVMuint32 max_items = official_size * INDEX_LOAD_FACTOR;
    MVMuint32 overflow_size = max_items - 1;
    /* -1 because...
     * probe distance of 1 is the correct bucket.
     * hence for a value whose ideal slot is the last bucket, it's *in* the
     * official allocation.
     * probe distance of 2 is the first extra bucket beyond the official
     * allocation
     * probe distance of 255 is the 254th beyond the official allocation.
     */
    MVMuint8 probe_overflow_size;
    if (MVM_HASH_MAX_PROBE_DISTANCE < overflow_size) {
        probe_overflow_size = MVM_HASH_MAX_PROBE_DISTANCE - 1;
    } else {
        probe_overflow_size = overflow_size;
    }
    size_t actual_items = official_size + probe_overflow_size;
    size_t entries_size = sizeof(struct MVMIndexHashEntry) * actual_items;
    size_t metadata_size = actual_items + 1;
    size_t total_size
        = entries_size + sizeof (struct MVMIndexHashTableControl) + metadata_size;

    struct MVMIndexHashTableControl *control =
        (struct MVMIndexHashTableControl *) ((char *)MVM_malloc(total_size) + entries_size);

    control->official_size = official_size;
    control->max_items = max_items;
    control->probe_overflow_size = probe_overflow_size;
    control->key_right_shift = key_right_shift;

    MVMuint8 *metadata = (MVMuint8 *)(control + 1);
    memset(metadata, 0, metadata_size);

    /* A sentinel. This marks an occupied slot, at its ideal position. */
    metadata[actual_items] = 1;

    return control;
}

void MVM_index_hash_build(MVMThreadContext *tc,
                          MVMIndexHashTable *hashtable,
                          MVMuint32 entries) {
    MVMuint32 initial_size_base2;
    if (!entries) {
        initial_size_base2 = INDEX_MIN_SIZE_BASE_2;
    } else {
        /* Minimum size we need to allocate, given the load factor. */
        MVMuint32 min_needed = entries * (1.0 / INDEX_LOAD_FACTOR);
        initial_size_base2 = MVM_round_up_log_base2(min_needed);
        if (initial_size_base2 < INDEX_MIN_SIZE_BASE_2) {
            /* "Too small" - use our original defaults. */
            initial_size_base2 = INDEX_MIN_SIZE_BASE_2;
        }
    }

    struct MVMIndexHashTableControl *control
        = hash_allocate_common(tc,
                               (8 * sizeof(MVMuint64) - initial_size_base2),
                               1 << initial_size_base2);

    control->cur_items = 0;
    hashtable->table = control;
}

MVM_STATIC_INLINE void hash_insert_internal(MVMThreadContext *tc,
                                            struct MVMIndexHashTableControl *control,
                                            MVMString **list,
                                            MVMuint32 idx) {
    if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        MVM_oops(tc, "oops, attempt to recursively call grow when adding %i",
                 idx);
    }

    unsigned int probe_distance = 1;
    MVMuint64 hash_val = MVM_string_hash_code(tc, list[idx]);
    MVMHashNumItems bucket = hash_val >> control->key_right_shift;
    MVMuint8 *entry_raw = MVM_index_hash_entries(control) - bucket * sizeof(struct MVMIndexHashEntry);
    MVMuint8 *metadata = MVM_index_hash_metadata(control) + bucket;
    while (1) {
        if (*metadata < probe_distance) {
            /* this is our slot. occupied or not, it is our rightful place. */

            if (*metadata == 0) {
                /* Open goal. Score! */
            } else {
                /* make room. */

                /* Optimisation first seen in Martin Ankerl's implementation -
                   we don't need actually implement the "stealing" by swapping
                   elements and carrying on with insert. The invariant of the
                   hash is that probe distances are never out of order, and as
                   all the following elements have probe distances in order, we
                   can maintain the invariant just as well by moving everything
                   along by one. */
                MVMuint8 *find_me_a_gap = metadata;
                MVMuint8 old_probe_distance = *metadata;
                do {
                    MVMuint8 new_probe_distance = 1 + old_probe_distance;
                    if (new_probe_distance == MVM_HASH_MAX_PROBE_DISTANCE) {
                        /* Optimisation from Martin Ankerl's implementation:
                           setting this to zero forces a resize on any insert,
                           *before* the actual insert, so that we never end up
                           having to handle overflow *during* this loop. This
                           loop can always complete. */
                        control->max_items = 0;
                    }
                    /* a swap: */
                    old_probe_distance = *++find_me_a_gap;
                    *find_me_a_gap = new_probe_distance;
                } while (old_probe_distance);

                MVMuint32 entries_to_move = find_me_a_gap - metadata;
                size_t size_to_move = sizeof(struct MVMIndexHashEntry) * entries_to_move;
                /* When we had entries *ascending* this was
                 * memmove(entry_raw + sizeof(struct MVMIndexHashEntry), entry_raw,
                 *         sizeof(struct MVMIndexHashEntry) * entries_to_move);
                 * because we point to the *start* of the block of memory we
                 * want to move, and we want to move it one "entry" forwards.
                 * `entry_raw` is still a pointer to where we want to make free
                 * space, but what want to do now is move everything at it and
                 * *before* it downwards. */
                MVMuint8 *dest = entry_raw - size_to_move;
                memmove(dest, dest + sizeof(struct MVMIndexHashEntry), size_to_move);
            }

            *metadata = probe_distance;
            struct MVMIndexHashEntry *entry = (struct MVMIndexHashEntry *) entry_raw;
            entry->index = idx;

            return;
        }

        if (*metadata == probe_distance) {
            struct MVMIndexHashEntry *entry = (struct MVMIndexHashEntry *) entry_raw;
            if (entry->index == idx) {
                /* definately XXX - what should we do here? */
                MVM_oops(tc, "insert duplicate for %u", idx);
            }
        }
        ++probe_distance;
        ++metadata;
        entry_raw -= sizeof(struct MVMIndexHashEntry);
        assert(probe_distance <= MVM_HASH_MAX_PROBE_DISTANCE);
        assert(metadata < MVM_index_hash_metadata(control) + control->official_size + control->max_items);
        assert(metadata < MVM_index_hash_metadata(control) + control->official_size + 256);
    }
}

/* UNCONDITIONALLY creates a new hash entry with the given key and value.
 * Doesn't check if the key already exists. Use with care. */
void MVM_index_hash_insert_nocheck(MVMThreadContext *tc,
                                   MVMIndexHashTable *hashtable,
                                   MVMString **list,
                                   MVMuint32 idx) {
    struct MVMIndexHashTableControl *control = hashtable->table;
    assert(control);
    assert(MVM_index_hash_entries(control) != NULL);
    if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        MVMuint32 true_size =  MVM_index_hash_kompromat(control);
        MVMuint8 *entry_raw_orig = MVM_index_hash_entries(control);
        MVMuint8 *metadata_orig = MVM_index_hash_metadata(control);

        struct MVMIndexHashTableControl *control_orig = control;

        control = hash_allocate_common(tc,
                                       control_orig->key_right_shift - 1,
                                       control_orig->official_size * 2);

        control->cur_items = control_orig->cur_items;
        hashtable->table = control;

        MVMuint8 *entry_raw = entry_raw_orig;
        MVMuint8 *metadata = metadata_orig;
        MVMHashNumItems bucket = 0;
        while (bucket < true_size) {
            if (*metadata) {
                struct MVMIndexHashEntry *entry = (struct MVMIndexHashEntry *) entry_raw;
                hash_insert_internal(tc, control, list, entry->index);
            }
            ++bucket;
            ++metadata;
            entry_raw -= sizeof(struct MVMIndexHashEntry);
        }
        hash_demolish_internal(tc, control_orig);
    }
    hash_insert_internal(tc, control, list, idx);
    ++control->cur_items;
}
