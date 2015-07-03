#include "moar.h"

/* Gets the current thread's profiling data structure, creating it if needed. */
static MVMProfileThreadData * get_thread_data(MVMThreadContext *tc) {
    if (!tc->prof_data) {
        tc->prof_data = MVM_calloc(1, sizeof(MVMProfileThreadData));
        tc->prof_data->start_time = uv_hrtime();
    }
    return tc->prof_data;
}

/* Log that we're entering a new frame. */
void MVM_profile_log_enter(MVMThreadContext *tc, MVMStaticFrame *sf, MVMuint64 mode) {
    MVMProfileThreadData *ptd = get_thread_data(tc);

    /* Try to locate the entry node, if it's in the call graph already. */
    MVMProfileCallNode *pcn = NULL;
    MVMuint32 i;
    if (ptd->current_call)
        for (i = 0; i < ptd->current_call->num_succ; i++)
            if (ptd->current_call->succ[i]->sf == sf)
                pcn = ptd->current_call->succ[i];

    /* If we didn't find a call graph node, then create one and add it to the
     * graph. */
    if (!pcn) {
        pcn     = MVM_calloc(1, sizeof(MVMProfileCallNode));
        pcn->sf = sf;
        if (ptd->current_call) {
            MVMProfileCallNode *pred = ptd->current_call;
            pcn->pred = pred;
            if (pred->num_succ == pred->alloc_succ) {
                pred->alloc_succ += 8;
                pred->succ = MVM_realloc(pred->succ,
                    pred->alloc_succ * sizeof(MVMProfileCallNode *));
            }
            pred->succ[pred->num_succ] = pcn;
            pred->num_succ++;
        }
        else {
            if (!ptd->call_graph)
                ptd->call_graph = pcn;
        }
    }

    /* Increment entry counts. */
    pcn->total_entries++;
    switch (mode) {
        case MVM_PROFILE_ENTER_SPESH:
            pcn->specialized_entries++;
            break;
        case MVM_PROFILE_ENTER_SPESH_INLINE:
            pcn->specialized_entries++;
            pcn->inlined_entries++;
            break;
        case MVM_PROFILE_ENTER_JIT:
            pcn->jit_entries++;
            break;
        case MVM_PROFILE_ENTER_JIT_INLINE:
            pcn->jit_entries++;
            pcn->inlined_entries++;
            break;
    }
    pcn->entry_mode = mode;

    /* Log entry time; clear skip time. */
    pcn->cur_entry_time = uv_hrtime();
    pcn->cur_skip_time  = 0;

    /* The current call graph node becomes this one. */
    ptd->current_call = pcn;
}

/* Frame exit handler, used for unwind and normal exit. */
static void log_exit(MVMThreadContext *tc, MVMuint32 unwind) {
    MVMProfileThreadData *ptd = get_thread_data(tc);

    /* Ensure we've a current frame; panic if not. */
    /* XXX in future, don't panic, try to cope. This is for debugging
     * profiler issues. */
    MVMProfileCallNode *pcn = ptd->current_call;
    if (!pcn /*|| !unwind && pcn->sf != tc->cur_frame->static_info*/) {
        MVM_dump_backtrace(tc);
        MVM_panic(1, "Profiler lost sequence");
    }

    /* Add to total time. */
    pcn->total_time += (uv_hrtime() - pcn->cur_entry_time) - pcn->cur_skip_time;

    /* Move back to predecessor in call graph. */
    ptd->current_call = pcn->pred;
}

/* Log that we're exiting a frame normally. */
void MVM_profile_log_exit(MVMThreadContext *tc) {
    log_exit(tc, 0);
}

/* Called when we unwind. Since we're also potentially leaving some inlined
 * frames, we need to exit until we hit the target one. */
void MVM_profile_log_unwind(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd  = get_thread_data(tc);
    MVMProfileCallNode   *lpcn;
    do {
        MVMProfileCallNode *pcn  = ptd->current_call;
        if (!pcn)
            return;
        lpcn = pcn;
        log_exit(tc, 1);
    } while (lpcn->sf != tc->cur_frame->static_info);
}

/* Called when we take a continuation. Leaves the static frames from the point
 * of view of the profiler, and saves each of them. */
MVMProfileContinuationData * MVM_profile_log_continuation_control(MVMThreadContext *tc, MVMFrame *root_frame) {
    MVMProfileThreadData        *ptd       = get_thread_data(tc);
    MVMProfileContinuationData  *cd        = MVM_malloc(sizeof(MVMProfileContinuationData));
    MVMStaticFrame             **sfs       = NULL;
    MVMuint64                   *modes     = NULL;
    MVMFrame                    *cur_frame = tc->cur_frame;
    MVMuint64                    alloc_sfs = 0;
    MVMuint64                    num_sfs   = 0;
    MVMFrame                   *last_frame;

    do {
        MVMProfileCallNode   *lpcn;
        do {
            MVMProfileCallNode *pcn = ptd->current_call;
            if (!pcn)
                MVM_panic(1, "Profiler lost sequence in continuation control");

            if (num_sfs == alloc_sfs) {
                alloc_sfs += 16;
                sfs        = MVM_realloc(sfs, alloc_sfs * sizeof(MVMStaticFrame *));
                modes      = MVM_realloc(modes, alloc_sfs * sizeof(MVMuint64));
            }
            sfs[num_sfs]   = pcn->sf;
            modes[num_sfs] = pcn->entry_mode;
            num_sfs++;

            lpcn = pcn;
            log_exit(tc, 1);
        } while (lpcn->sf != cur_frame->static_info);

        last_frame = cur_frame;
        cur_frame = cur_frame->caller;
    } while (last_frame != root_frame);

    cd->sfs     = sfs;
    cd->num_sfs = num_sfs;
    cd->modes   = modes;
    return cd;
}

/* Called when we invoke a continuation. Enters all the static frames we left
 * at the point we took the continuation. */
void MVM_profile_log_continuation_invoke(MVMThreadContext *tc, MVMProfileContinuationData *cd) {
    MVMuint64 i = cd->num_sfs;
    while (i--)
        MVM_profile_log_enter(tc, cd->sfs[i], cd->modes[i]);
}

/* Log that we've just allocated the passed object (just log the type). */
void MVM_profile_log_allocated(MVMThreadContext *tc, MVMObject *obj) {
    MVMProfileThreadData *ptd  = get_thread_data(tc);
    MVMProfileCallNode   *pcn  = ptd->current_call;
    if (pcn) {
        /* First, let's see if the allocation is actually at the end of the
         * nursery; we may have generated some "allocated" log instructions
         * after operations that may or may not allocate what they return.
         */
        MVMuint32 distance = ((MVMuint64)tc->nursery_alloc - (MVMuint64)obj);

        if (!obj) {
            return;
        }

        /* Since some ops first allocate, then call something else that may
         * also allocate, we may have to allow for a bit of grace distance. */
        if ((MVMuint64)obj > (MVMuint64)tc->nursery_tospace && distance <= obj->header.size && obj != ptd->last_counted_allocation) {
            /* See if there's an existing node to update. */
            MVMObject            *what = STABLE(obj)->WHAT;
            MVMuint32 i;

            MVMuint8 allocation_target;
            if (pcn->entry_mode == MVM_PROFILE_ENTER_SPESH || pcn->entry_mode == MVM_PROFILE_ENTER_SPESH_INLINE) {
                allocation_target = 1;
            } else if (pcn->entry_mode == MVM_PROFILE_ENTER_JIT || pcn->entry_mode == MVM_PROFILE_ENTER_JIT_INLINE) {
                allocation_target = 2;
            } else {
                allocation_target = 0;
            }

            for (i = 0; i < pcn->num_alloc; i++) {
                if (pcn->alloc[i].type == what) {
                    if (allocation_target == 0)
                        pcn->alloc[i].allocations_interp++;
                    else if (allocation_target == 1)
                        pcn->alloc[i].allocations_spesh++;
                    else if (allocation_target == 2)
                        pcn->alloc[i].allocations_jit++;
                    ptd->last_counted_allocation = obj;

                    goto add_tracking_info;
                }
            }

            /* No entry; create one. */
            if (pcn->num_alloc == pcn->alloc_alloc) {
                pcn->alloc_alloc += 8;
                pcn->alloc = MVM_realloc(pcn->alloc,
                    pcn->alloc_alloc * sizeof(MVMProfileAllocationCount));
            }
            pcn->alloc[pcn->num_alloc].type        = what;
            pcn->alloc[pcn->num_alloc].allocations_interp = allocation_target == 0;
            pcn->alloc[pcn->num_alloc].allocations_spesh  = allocation_target == 1;
            pcn->alloc[pcn->num_alloc].allocations_jit    = allocation_target == 2;
            pcn->alloc[pcn->num_alloc].allocations_jit    = allocation_target == 2;
            pcn->alloc[pcn->num_alloc].dead_before_gen2   = 0;
            ptd->last_counted_allocation = obj;
            pcn->num_alloc++;

add_tracking_info:
            /* And now track the object for later use */
            if (ptd->num_tracked == ptd->alloc_tracked) {
                ptd->alloc_tracked += 100;
                ptd->tracked_objects = MVM_realloc(ptd->tracked_objects, ptd->alloc_tracked * sizeof(MVMObject**));
                ptd->tracked_nodes = MVM_realloc(ptd->tracked_nodes, ptd->alloc_tracked * sizeof(MVMProfileCallNode**));
                ptd->tracked_node_alloc_slots = MVM_realloc(ptd->tracked_node_alloc_slots, ptd->alloc_tracked * sizeof(MVMuint32));
            }
            ptd->tracked_objects[ptd->num_tracked] = obj;
            ptd->tracked_nodes[ptd->num_tracked] = pcn;
            ptd->tracked_node_alloc_slots[ptd->num_tracked] = pcn->num_alloc - 1;
            ptd->num_tracked++;

        }
    }
}

void MVM_profiler_scan_tracked_objects(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd  = get_thread_data(tc);
    MVMuint32 idx, free_spot;
    MVMuint32 new_tracked = ptd->num_tracked;

    for (idx = 0; idx < ptd->num_tracked; idx++) {
        MVMCollectable *obj = (MVMCollectable*)ptd->tracked_objects[idx];
        if (obj->flags & MVM_CF_FORWARDER_VALID) {
            /* Let's check if it got copied to the new nursery */
            if ((char *)obj < (char*)tc->nursery_alloc_limit && (char*)obj > (char*)tc->nursery_alloc_limit - MVM_NURSERY_SIZE) {
                /* Update this entry to point to the object in the
                 * next nursery */
                ptd->tracked_objects[idx] = (MVMObject*)obj->sc_forward_u.forwarder;
            } else {
                /* Throw this object out, now that it is in the gen2 */
                ptd->tracked_objects[idx] = NULL;
                new_tracked--;
            }
        } else {
            MVMProfileCallNode *callnode = ptd->tracked_nodes[idx];
            MVMuint32 callnode_alloc_slot = ptd->tracked_node_alloc_slots[idx];
            MVMProfileAllocationCount *alloc_count = &(callnode->alloc[callnode_alloc_slot]);
            alloc_count->dead_before_gen2++;

            ptd->tracked_objects[idx] = NULL;
            new_tracked--;
        }
    }

    for (idx = 0; idx < ptd->num_tracked; idx++) {
        if (ptd->tracked_objects[idx] == NULL) {
            free_spot = idx;
            break;
        }
    }
    idx++;
    for (; idx < ptd->num_tracked; idx++) {
        if (ptd->tracked_objects[idx] != NULL) {
            ptd->tracked_objects[free_spot] = ptd->tracked_objects[idx];
            ptd->tracked_nodes[free_spot] = ptd->tracked_nodes[idx];
            ptd->tracked_node_alloc_slots[free_spot] = ptd->tracked_node_alloc_slots[idx];

            ptd->tracked_objects[idx] = NULL;

            while (ptd->tracked_objects[free_spot] != NULL && free_spot <= idx)
                free_spot++;
        }
    }

    ptd->num_tracked = new_tracked;
}

/* Logs the start of a GC run. */
void MVM_profiler_log_gc_start(MVMThreadContext *tc, MVMuint32 full) {
    MVMProfileThreadData *ptd = get_thread_data(tc);

    /* Make a new entry in the GCs. We use the cleared_bytes to store the
     * maximum that could be cleared, and after GC is done will subtract
     * retained bytes and promoted bytes. */
    if (ptd->num_gcs == ptd->alloc_gcs) {
        ptd->alloc_gcs += 16;
        ptd->gcs = MVM_realloc(ptd->gcs, ptd->alloc_gcs * sizeof(MVMProfileGC));
    }
    ptd->gcs[ptd->num_gcs].full          = full;
    ptd->gcs[ptd->num_gcs].cleared_bytes = (char *)tc->nursery_alloc -
                                           (char *)tc->nursery_tospace;

    /* Record start time. */
    ptd->cur_gc_start_time = uv_hrtime();
}

/* Logs the end of a GC run. */
void MVM_profiler_log_gc_end(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    MVMuint64 gc_time;
    MVMint32  retained_bytes;

    /* Record time spent. */
    gc_time = uv_hrtime() - ptd->cur_gc_start_time;
    ptd->gcs[ptd->num_gcs].time = gc_time;

    /* Record retained and promoted bytes. */
    retained_bytes = (char *)tc->nursery_alloc - (char *)tc->nursery_tospace;
    ptd->gcs[ptd->num_gcs].promoted_bytes = tc->gc_promoted_bytes;
    ptd->gcs[ptd->num_gcs].retained_bytes = retained_bytes;

    /* Tweak cleared bytes count. */
    ptd->gcs[ptd->num_gcs].cleared_bytes -= (retained_bytes + tc->gc_promoted_bytes);

    /* Record number of gen 2 roots (from gen2 to nursery) */
    ptd->gcs[ptd->num_gcs].num_gen2roots = tc->num_gen2roots;

    /* Increment the number of GCs we've done. */
    ptd->num_gcs++;

    /* Discount GC time from all active frames. */
    while (pcn) {
        pcn->cur_skip_time += gc_time;
        pcn = pcn->pred;
    }
}

/* Log that we're starting some work on bytecode specialization or JIT. */
void MVM_profiler_log_spesh_start(MVMThreadContext *tc) {
    /* Record start time. */
    MVMProfileThreadData *ptd = get_thread_data(tc);
    ptd->cur_spesh_start_time = uv_hrtime();
}

/* Log that we've finished doing bytecode specialization or JIT. */
void MVM_profiler_log_spesh_end(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    MVMuint64 spesh_time;

    /* Record time spent. */
    spesh_time = uv_hrtime() - ptd->cur_spesh_start_time;
    ptd->spesh_time += spesh_time;

    /* Discount spesh time from all active frames. */
    while (pcn) {
        pcn->cur_skip_time += spesh_time;
        pcn = pcn->pred;
    }
}

/* Log that an on stack replacement took place. */
void MVM_profiler_log_osr(MVMThreadContext *tc, MVMuint64 jitted) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    if (pcn) {
        pcn->osr_count++;
        if (jitted)
            pcn->jit_entries++;
        else
            pcn->specialized_entries++;
    }
}

/* Log that local deoptimization took pace. */
void MVM_profiler_log_deopt_one(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    if (pcn)
        pcn->deopt_one_count++;
}

/* Log that full-stack deoptimization took pace. */
void MVM_profiler_log_deopt_all(MVMThreadContext *tc) {
    MVMProfileThreadData *ptd = get_thread_data(tc);
    MVMProfileCallNode   *pcn = ptd->current_call;
    if (pcn)
        pcn->deopt_all_count++;
}
