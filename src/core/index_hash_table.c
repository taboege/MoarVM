#include "moar.h"

#define INDEX_LOAD_FACTOR 0.75
#define INDEX_MIN_SIZE_BASE_2 3

MVM_STATIC_INLINE MVMuint32 hash_true_size(const struct MVMIndexHashTableControl *control) {
    MVMuint32 true_size = control->official_size + control->max_items - 1;
    if (control->official_size + MVM_HASH_MAX_PROBE_DISTANCE < true_size) {
        true_size = control->official_size + MVM_HASH_MAX_PROBE_DISTANCE;
    }
    return true_size;
}

/* Frees the entire contents of the hash, leaving you just the hashtable itself,
   which you allocated (heap, stack, inside another struct, wherever) */
void MVM_index_hash_demolish(MVMThreadContext *tc, MVMIndexHashTable *hashtable) {
    struct MVMIndexHashTableControl *control = hashtable->table;
    if (!control)
        return;
    if (control->entries) {
        MVM_free(control->entries
                 - sizeof(struct MVMIndexHashEntry) * (hash_true_size(control) - 1));
    }
    MVM_free(control);
    hashtable->table = NULL;
}
/* and then free memory if you allocated it */


MVM_STATIC_INLINE void hash_allocate_common(struct MVMIndexHashTableControl *control) {
    control->max_items = control->official_size * INDEX_LOAD_FACTOR;
    size_t actual_items = hash_true_size(control);
    size_t entries_size = sizeof(struct MVMIndexHashEntry) * actual_items;
    size_t metadata_size = 1 + actual_items + 1;
    control->metadata
        = (MVMuint8 *) MVM_malloc(entries_size + metadata_size) + entries_size;
    memset(control->metadata, 0, metadata_size);
    /* We point to the *last* entry in the array, not the one-after-the end. */
    control->entries = control->metadata - sizeof(struct MVMIndexHashEntry);
    /* A sentinel. This marks an occupied slot, at its ideal position. */
    *control->metadata = 1;
    ++control->metadata;
    /* A sentinel at the other end. Again, occupied, ideal position. */
    control->metadata[actual_items] = 1;
}

void MVM_index_hash_build(MVMThreadContext *tc,
                          MVMIndexHashTable *hashtable,
                          MVMuint32 entries) {
    struct MVMIndexHashTableControl *control = MVM_calloc(1,sizeof(struct MVMIndexHashTableControl));
    hashtable->table = control;

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

    control->key_right_shift = (8 * sizeof(MVMuint64) - initial_size_base2);
    control->official_size = 1 << initial_size_base2;

    hash_allocate_common(control);
}

/* make sure you still have your copies of entries and metadata before you
   call this. */
MVM_STATIC_INLINE void hash_grow(struct MVMIndexHashTableControl *control) {
    --control->key_right_shift;
    control->official_size *= 2;

    hash_allocate_common(control);
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
        MVMuint32 true_size =  hash_true_size(control);
        MVMuint8 *entry_raw_orig = MVM_index_hash_entries(control);
        MVMuint8 *metadata_orig = MVM_index_hash_metadata(control);

        hash_grow(control);

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
        MVM_free(entry_raw_orig - sizeof(struct MVMIndexHashEntry) * (true_size - 1));
    }
    hash_insert_internal(tc, control, list, idx);
    ++control->cur_items;
}
