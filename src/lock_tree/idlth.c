/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007-8 Tokutek Inc.  All rights reserved."

#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

/**
   \file  hash_idlth.h
   \brief Hash idlth
  
*/

#include "toku_portability.h"
#include <idlth.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

/* TODO: investigate whether we can remove the user_memory functions */
/* TODO: reallocate the hash idlth if it grows too big. Perhaps, use toku_get_prime in newbrt/primes.c */
const u_int32_t __toku_idlth_init_size = 521;

static inline u_int32_t toku__idlth_hash(toku_idlth* idlth, toku_db_id* key) {
    size_t tmp = key->saved_hash;
    return tmp % idlth->num_buckets;
}

static inline void toku__invalidate_scan(toku_idlth* idlth) {
    idlth->iter_is_valid = FALSE;
}

int toku_idlth_create(toku_idlth** pidlth,
                      void* (*user_malloc) (size_t),
                      void  (*user_free)   (void*),
                      void* (*user_realloc)(void*, size_t)) {
    int r = ENOSYS;
    assert(pidlth && user_malloc && user_free && user_realloc);
    toku_idlth* tmp = NULL;
    tmp = (toku_idlth*)user_malloc(sizeof(*tmp));
    if (!tmp) { r = ENOMEM; goto cleanup; }

    memset(tmp, 0, sizeof(*tmp));
    tmp->malloc      = user_malloc;
    tmp->free        = user_free;
    tmp->realloc     = user_realloc;
    tmp->num_buckets = __toku_idlth_init_size;
    tmp->buckets     = (toku_idlth_elt*)
                          tmp->malloc(tmp->num_buckets * sizeof(*tmp->buckets));
    if (!tmp->buckets) { r = ENOMEM; goto cleanup; }
    memset(tmp->buckets, 0, tmp->num_buckets * sizeof(*tmp->buckets));
    toku__invalidate_scan(tmp);
    tmp->iter_head.next_in_iteration = &tmp->iter_head;
    tmp->iter_head.prev_in_iteration = &tmp->iter_head;

    *pidlth = tmp;
    r = 0;
cleanup:
    if (r != 0) {
        if (tmp) {
            if (tmp->buckets) { user_free(tmp->buckets); }
            user_free(tmp);
        }
    }
    return r;
}

toku_lt_map* toku_idlth_find(toku_idlth* idlth, toku_db_id* key) {
    assert(idlth);

    u_int32_t index         = toku__idlth_hash(idlth, key);
    toku_idlth_elt* head    = &idlth->buckets[index];
    toku_idlth_elt* current = head->next_in_bucket;
    while (current) {
        if (toku_db_id_equals(current->value.db_id, key)) { break; }
        current = current->next_in_bucket;
    }
    return current ? &current->value : NULL;
}

void toku_idlth_start_scan(toku_idlth* idlth) {
    assert(idlth);
    idlth->iter_curr = &idlth->iter_head;
    idlth->iter_is_valid = TRUE;
}

static inline toku_idlth_elt* toku__idlth_next(toku_idlth* idlth) {
    assert(idlth);
    assert(idlth->iter_is_valid);

    idlth->iter_curr     = idlth->iter_curr->next_in_iteration;
    idlth->iter_is_valid = (BOOL)(idlth->iter_curr != &idlth->iter_head);
    return idlth->iter_curr;
}

toku_lt_map* toku_idlth_next(toku_idlth* idlth) {
    assert(idlth);
    toku_idlth_elt* next = toku__idlth_next(idlth);
    return idlth->iter_curr != &idlth->iter_head ? &next->value : NULL;
}

/* Element MUST exist. */
void toku_idlth_delete(toku_idlth* idlth, toku_db_id* key) {
    assert(idlth);
    toku__invalidate_scan(idlth);

    /* Must have elements. */
    assert(idlth->num_keys);

    u_int32_t index = toku__idlth_hash(idlth, key);
    toku_idlth_elt* head    = &idlth->buckets[index]; 
    toku_idlth_elt* prev    = head; 
    toku_idlth_elt* current = prev->next_in_bucket;

    while (current != NULL) {
        if (toku_db_id_equals(current->value.db_id, key)) { break; }
        prev = current;
        current = current->next_in_bucket;
    }
    /* Must be found. */
    assert(current);
    current->prev_in_iteration->next_in_iteration = current->next_in_iteration;
    current->next_in_iteration->prev_in_iteration = current->prev_in_iteration;
    prev->next_in_bucket = current->next_in_bucket;
    toku_db_id_remove_ref(&current->value.db_id);
    idlth->free(current);
    idlth->num_keys--;
    return;
}
    
/* Will allow you to insert it over and over.  You need to keep track. */
int toku_idlth_insert(toku_idlth* idlth, toku_db_id* key) {
    int r = ENOSYS;
    assert(idlth);
    toku__invalidate_scan(idlth);

    u_int32_t index = toku__idlth_hash(idlth, key);

    /* Allocate a new one. */
    toku_idlth_elt* element = (toku_idlth_elt*)idlth->malloc(sizeof(*element));
    if (!element) { r = ENOMEM; goto cleanup; }
    memset(element, 0, sizeof(*element));
    element->value.db_id = key;
    toku_db_id_add_ref(element->value.db_id);

    element->next_in_iteration = idlth->iter_head.next_in_iteration;
    element->prev_in_iteration = &idlth->iter_head;
    element->next_in_iteration->prev_in_iteration = element;
    element->prev_in_iteration->next_in_iteration = element;
    
    element->next_in_bucket = idlth->buckets[index].next_in_bucket;
    idlth->buckets[index].next_in_bucket = element;
    idlth->num_keys++;

    r = 0;
cleanup:
    return r;    
}

static inline void toku__idlth_clear(toku_idlth* idlth, BOOL clean) {
    assert(idlth);

    toku_idlth_elt* element;
    toku_idlth_elt* head = &idlth->iter_head;
    toku_idlth_elt* next = NULL;
    toku_idlth_start_scan(idlth);
    next = toku__idlth_next(idlth);
    while (next != head) {
        element = next;
        next    = toku__idlth_next(idlth);
        toku_db_id_remove_ref(&element->value.db_id);
        idlth->free(element);
    }
    /* If clean is true, then we want to restore it to 'just created' status.
       If we are closing the tree, we don't need to do that restoration. */
    if (!clean) { return; }
    memset(idlth->buckets, 0, idlth->num_buckets * sizeof(*idlth->buckets));
    toku__invalidate_scan(idlth);
    idlth->iter_head.next_in_iteration = &idlth->iter_head;
    idlth->iter_head.prev_in_iteration = &idlth->iter_head;
    idlth->num_keys = 0;
}

void toku_idlth_clear(toku_idlth* idlth) {
    toku__idlth_clear(idlth, TRUE);
}

void toku_idlth_close(toku_idlth* idlth) {
    assert(idlth);

    toku__idlth_clear(idlth, FALSE);
    idlth->free(idlth->buckets);
    idlth->free(idlth);
}

BOOL toku_idlth_is_empty(toku_idlth* idlth) {
    assert(idlth);
    /* Verify consistency. */
    assert((idlth->num_keys == 0) ==
           (idlth->iter_head.next_in_iteration == &idlth->iter_head));
    assert((idlth->num_keys == 0) ==
           (idlth->iter_head.prev_in_iteration == &idlth->iter_head));
    return (BOOL)(idlth->num_keys == 0);
}
