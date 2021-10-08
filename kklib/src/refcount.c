/*---------------------------------------------------------------------------
  Copyright 2020-2021, Microsoft Research, Daan Leijen, Anton Lorenzen

  This is free software; you can redistribute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the LICENSE file at the root of this distribution.
---------------------------------------------------------------------------*/
#include "kklib.h"

static void kk_block_drop_free_delayed(kk_context_t* ctx);
static kk_decl_noinline void kk_block_drop_free_visitor(kk_block_t* b, kk_context_t* ctx);
static kk_decl_noinline void kk_block_drop_free_rec(kk_block_t* b, kk_ssize_t scan_fsize, const kk_ssize_t depth, kk_context_t* ctx);

static void kk_block_free_raw(kk_block_t* b, kk_context_t* ctx) {
  kk_assert_internal(kk_tag_is_raw(kk_block_tag(b)));
  struct kk_cptr_raw_s* raw = (struct kk_cptr_raw_s*)b;  // all raw structures must overlap this!
  if (raw->free != NULL) {
    (*raw->free)(raw->cptr, b, ctx);
  }
}

// Free a block and recursively decrement reference counts on children.
static void kk_block_drop_free(kk_block_t* b, kk_context_t* ctx) {
  kk_assert_internal(b->header.refcount == 0);
  const kk_ssize_t scan_fsize = b->header.scan_fsize;
  if (scan_fsize==0) {
    if (kk_tag_is_raw(kk_block_tag(b))) { kk_block_free_raw(b,ctx); }
    kk_block_free(b); // deallocate directly if nothing to scan
  }
  else {
    // kk_block_drop_free_visitor(b, ctx);
    kk_block_drop_free_rec(b, scan_fsize, 0 /* depth */, ctx);  // free recursively
    kk_block_drop_free_delayed(ctx);     // process delayed frees
  }
}



/*--------------------------------------------------------------------------------------
  Checked reference counts. 
  - We use a sticky range above `RC_STICKY_LO` to prevent overflow
    of the reference count. Any sticky reference won't be freed. There is a range between
    `RC_STICKY_LO` and `RC_STICKY_HI` to ensure stickyness even with concurrent increments and decrements.
  - The range above `RC_SHARED` uses atomic operations for shared reference counts. If a decrement
    falls to `RC_SHARED` the object is freed (if is actually was shared, i.e. `kk_thread_shared` is true).
  - Since `RC_SHARED` has the msb set, we can efficiently test in `drop` for either `0` (=free) or
    the need for atomic operations by using `if ((int32_t)rc <= 0) ...` (and similarly for `dup`).
  
  0                         : unique reference
  0x00000001 - 0x7FFFFFFF   : reference (in a single thread)
  0x80000000 - 0xCFFFFFFF   : reference or thread-shared reference (if `kk_thread_shared`). Use atomic operations
  0xD0000000 - 0xDFFFFFFF   : sticky range: still increments, but no decrements
  0xE0000000 - 0xEFFFFFFF   : sticky range: neither increment, nor decrement
  0xF0000000 - 0xFFFFFFFF   : invalid; used for debug checks
--------------------------------------------------------------------------------------*/

#define RC_SHARED     KU32(0x80000000)  // 0b1000 ...
#define RC_STICKY_LO  KU32(0xD0000000)  // 0b1101 ...
#define RC_STICKY_HI  KU32(0xE0000000)  // 0b1110 ...
#define RC_INVALID    KU32(0xF0000000)  // 0b1111 ...

static inline uint32_t kk_atomic_incr(kk_block_t* b) {
  return kk_atomic_inc32_relaxed((_Atomic(uint32_t)*)&b->header.refcount);
}
static inline uint32_t kk_atomic_decr(kk_block_t* b) {
  return kk_atomic_dec32_relaxed((_Atomic(uint32_t)*)&b->header.refcount);
}
static void kk_block_make_shared(kk_block_t* b) {
  b->header.thread_shared = true;
  kk_atomic_add32_relaxed((_Atomic(uint32_t)*)&b->header.refcount, RC_SHARED+1);
}

// Check if a reference decrement caused the block to be free or needs atomic operations
kk_decl_noinline void kk_block_check_drop(kk_block_t* b, uint32_t rc0, kk_context_t* ctx) {
  kk_assert_internal(b!=NULL);
  kk_assert_internal(b->header.refcount == rc0);
  kk_assert_internal(rc0 == 0 || (rc0 >= RC_SHARED && rc0 < RC_INVALID));
  if (kk_likely(rc0==0)) {
    kk_block_drop_free(b, ctx);  // no more references, free it.
  }
  else if (kk_unlikely(rc0 >= RC_STICKY_LO)) {
    // sticky: do not decrement further
  }
  else {
    const uint32_t rc = kk_atomic_decr(b);
    if (rc == RC_SHARED && b->header.thread_shared) {  // with a shared reference dropping to RC_SHARED means no more references
      b->header.refcount = 0;        // no longer shared
      b->header.thread_shared = 0;
      kk_block_drop_free(b, ctx);            // no more references, free it.
    }
  }
}

// Check if a reference decrement caused the block to be reused or needs atomic operations
kk_decl_noinline kk_reuse_t kk_block_check_drop_reuse(kk_block_t* b, uint32_t rc0, kk_context_t* ctx) {
  kk_assert_internal(b!=NULL);
  kk_assert_internal(b->header.refcount == rc0);
  kk_assert_internal(rc0 == 0 || (rc0 >= RC_SHARED && rc0 < RC_INVALID));
  if (kk_likely(rc0==0)) {
    // no more references, reuse it.
    kk_ssize_t scan_fsize = kk_block_scan_fsize(b);
    for (kk_ssize_t i = 0; i < scan_fsize; i++) {
      kk_box_drop(kk_block_field(b, i), ctx);
    }
    memset(&b->header, 0, sizeof(kk_header_t)); // not really necessary
    return b;
  }
  else {
    // may be shared or sticky
    kk_block_check_drop(b, rc0, ctx);
    return kk_reuse_null;
  }
}

// Check if a reference decrement caused the block to be freed shallowly or needs atomic operations
kk_decl_noinline void kk_block_check_decref(kk_block_t* b, uint32_t rc0, kk_context_t* ctx) {
  KK_UNUSED(ctx);
  kk_assert_internal(b!=NULL);
  kk_assert_internal(b->header.refcount == rc0);
  kk_assert_internal(rc0 == 0 || (rc0 >= RC_SHARED && rc0 < RC_INVALID));
  if (kk_likely(rc0==0)) {
    kk_free(b);  // no more references, free it (without dropping children!)
  }
  else if (kk_unlikely(rc0 >= RC_STICKY_LO)) {
    // sticky: do not decrement further
  }
  else {
    const uint32_t rc = kk_atomic_decr(b);
    if (rc == RC_SHARED && b->header.thread_shared) {  // with a shared reference dropping to RC_SHARED means no more references
      b->header.refcount = 0;        // no longer shared
      b->header.thread_shared = 0;
      kk_free(b);               // no more references, free it.
    }
  }
}


kk_decl_noinline kk_block_t* kk_block_check_dup(kk_block_t* b, uint32_t rc0) {
  kk_assert_internal(b!=NULL);
  kk_assert_internal(b->header.refcount == rc0 && rc0 >= RC_SHARED);
  if (kk_likely(rc0 < RC_STICKY_HI)) {
    kk_atomic_incr(b);
  }
  // else sticky: no longer increment (or decrement)
  return b;
}


/*--------------------------------------------------------------------------------------
  Decrementing reference counts
  When freeing a block, we need to decrease reference counts of its children
  recursively. We carefully optimize to use no stack space in case of single field
  chains (like lists) and recurse to limited depth in other cases, using a
  `delayed_free` list in the thread local data. The `delayed_free` list is
  encoded in the headers and thus needs no allocation.
--------------------------------------------------------------------------------------*/

// Decrement a shared refcount without freeing the block yet. Returns true if there are no more references.
static bool block_check_decref_no_free(kk_block_t* b) {
  const uint32_t rc = kk_atomic_decr(b);
  if (rc == RC_SHARED && b->header.thread_shared) {
    b->header.refcount = 0;      // no more shared
    b->header.thread_shared = 0;   
    return true;                   // no more references
  }
  if (kk_unlikely(rc > RC_STICKY_LO)) {
    kk_atomic_incr(b);                // sticky: undo the decrement to never free
  }
  return false;  
}

// Decrement a refcount without freeing the block yet. Returns true if there are no more references.
static bool kk_block_decref_no_free(kk_block_t* b) {
  uint32_t rc = b->header.refcount;
  if (rc==0) return true;
  else if (rc >= RC_SHARED) return block_check_decref_no_free(b);
  b->header.refcount = rc - 1;
  return false;
}

// Free a block and decref all its children. Recursively free a child if
// the reference count of a child is zero. We apply the visitor pattern:
//  (1) We store the pointer to the parent in the
//      0th scan field (or the 1st if scan_fsize >= KK_SCAN_FSIZE_MAX).
//      It is NULL for the root.
//  (2) We store an integer that gives the next scan_field to free
//      in the refcount.
// Invariants:
//  (1) Any parent node still has at least one further scan field to process.
//      All nodes with zero or one scan field are freed directly.
//  (2) 'parent' is set correctly when moving down (but not when moving up).
//  (3) 'b' is NULL only when moving up beyond the root.
static kk_decl_noinline void kk_block_drop_free_visitor(kk_block_t* b, kk_context_t* ctx) {
  kk_block_t* parent = NULL;
  bool moving_up = false;
  while(true) {
    outer: ;
    if(moving_up) {
      if(b == NULL) return;
      kk_ssize_t scan_fsize = b->header.scan_fsize;
      kk_box_t parent_ptr = kk_block_field(b, 0);
      if (kk_unlikely(scan_fsize >= KK_SCAN_FSIZE_MAX)) { 
        scan_fsize = (kk_ssize_t)kk_int_unbox(kk_block_field(b, 0)) + 1;
        parent_ptr = kk_block_field(b, 1);
      }
      kk_ssize_t i = b->header.refcount;
      kk_box_t v = kk_block_field(b, i);
      scan_fsize -= 1;
      while(i != scan_fsize) {
        i++;
        if(kk_box_is_non_null_ptr(v)) {
          kk_block_t* next = kk_ptr_unbox(v);
          if (kk_block_decref_no_free(next)) {
            // free recursively
            b->header.refcount = i;
            parent = b;
            b = next;
            moving_up = false;
            goto outer;
          } // else: move on to next scan field
        }
        v = kk_block_field(b, i);
      } // else: work on last scan field
      kk_block_free(b);
      b = (kk_block_t*)(parent_ptr.box); // kk_ptr_unbox, but parent_ptr may be null
      if(kk_box_is_non_null_ptr(v)) {
        kk_block_t* next = kk_ptr_unbox(v);
        if (kk_block_decref_no_free(next)) {
          parent = b;
          b = next;
          moving_up = false;
          goto outer;
        } // else: go up
      }
      moving_up = true;
    }
    else { // moving down
      kk_ssize_t scan_fsize = b->header.scan_fsize;
      if(scan_fsize == 0) {
        // free and go up
        if (kk_tag_is_raw(kk_block_tag(b))) { kk_block_free_raw(b,ctx); }
        kk_block_free(b);
        b = parent;
        moving_up = true;
      }
      else if(scan_fsize == 1) {
        const kk_box_t v = kk_block_field(b, 0);
        kk_block_free(b);
        if (kk_box_is_non_null_ptr(v)) {
          b = kk_ptr_unbox(v);
          if (kk_block_decref_no_free(b)) {
            goto outer; // same b and parent, still moving down
          } // else: go up
        }
        b = parent;
        moving_up = true;
      }
      else {
        kk_ssize_t i = 0;
        if (kk_unlikely(scan_fsize >= KK_SCAN_FSIZE_MAX)) { 
          scan_fsize = (kk_ssize_t)kk_int_unbox(kk_block_field(b, 0)) + 1;
          i++;  // skip the scan field itself (and the full scan_fsize does not include the field itself)
        }
        kk_ssize_t parent_idx = i; // store the parent here
        kk_box_t v = kk_block_field(b, i);
        scan_fsize -= 1;
        while(i != scan_fsize) {
          i++;
          if(kk_box_is_non_null_ptr(v)) {
            kk_block_t* next = kk_ptr_unbox(v);
            if (kk_block_decref_no_free(next)) {
              b->header.refcount = i;
              kk_block_fields_t* bf = (kk_block_fields_t*)b; // assign to kk_block_field(b, parent_idx)
              bf->fields[parent_idx] = (kk_box_t) { (uintptr_t)(parent) }; // kk_ptr_box(parent), but parent may be null
              parent = b;
              b = next;
              moving_up = false;
              goto outer;
            } // else: move on to next scan field
          }
          v = kk_block_field(b, i);
        } // else: work on last scan field
        kk_block_free(b);
        if(kk_box_is_non_null_ptr(v)) {
          kk_block_t* next = kk_ptr_unbox(v);
          if (kk_block_decref_no_free(next)) {
            b = kk_ptr_unbox(v);
            // use old parent
            moving_up = false;
            goto outer;
          } // else: go up
        }
        b = parent;
        moving_up = true;
      }
    }
  }
}

// Push a block on the delayed-free list
static void kk_block_push_delayed_drop_free(kk_block_t* b, kk_context_t* ctx) {
  kk_assert_internal(b->header.refcount == 0);
  kk_block_t* delayed = ctx->delayed_free;
  // encode the next pointer into the block header (while keeping `scan_fsize` valid)
  b->header.refcount = (uint32_t)((uintptr_t)delayed);
#if (KK_INTPTR_SIZE > 4)
  b->header.tag = (uint16_t)((uintptr_t)delayed >> 32);  // at most 48 bits, but can extend to 56 bits (as only scan_fsize needs to be preserved)
  kk_assert_internal(((uintptr_t)delayed >> 48) == 0);   // adapt for sign extension?
#endif
  ctx->delayed_free = b;
}


// Free all delayed free blocks.
// TODO: limit to a certain number to limit worst-case free times?
static void kk_block_drop_free_delayed(kk_context_t* ctx) {
  kk_block_t* delayed;
  while ((delayed = ctx->delayed_free) != NULL) {
    ctx->delayed_free = NULL;
    do {
      kk_block_t* b = delayed;
      // decode the next element in the delayed list from the block header
      uintptr_t next = (uintptr_t)b->header.refcount;
#if (KK_INTPTR_SIZE>4)
      next += (uintptr_t)(b->header.tag) << 32; 
#endif
#ifndef NDEBUG
      b->header.refcount = 0;
#endif
      delayed = (kk_block_t*)next;
      // and free the block
      kk_block_drop_free_rec(b, b->header.scan_fsize, 0, ctx);
    } while (delayed != NULL);
  }
}

#define MAX_RECURSE_DEPTH (100)

// Free recursively a block -- if the recursion becomes too deep, push
// blocks on the delayed free list to free them later. The delayed free list
// is encoded in the headers and needs no further space.
static kk_decl_noinline void kk_block_drop_free_rec(kk_block_t* b, kk_ssize_t scan_fsize, const kk_ssize_t depth, kk_context_t* ctx) {
  while(true) {
    kk_assert_internal(b->header.refcount == 0);
    if (scan_fsize == 0) {
      // nothing to scan, just free
      if (kk_tag_is_raw(kk_block_tag(b))) kk_block_free_raw(b,ctx); // potentially call custom `free` function on the data
      kk_block_free(b);
      return;
    }
    else if (scan_fsize == 1) {
      // if just one field, we can recursively free without using stack space
      const kk_box_t v = kk_block_field(b, 0);
      kk_block_free(b);
      if (kk_box_is_non_null_ptr(v)) {
        // try to free the child now
        b = kk_ptr_unbox(v);
        if (kk_block_decref_no_free(b)) {
          // continue freeing on this block
          scan_fsize = b->header.scan_fsize;
          continue; // tailcall
        }
      }
      return;
    }
    else {
      // more than 1 field
      if (depth < MAX_RECURSE_DEPTH) {
        kk_ssize_t i = 0;
        if (kk_unlikely(scan_fsize >= KK_SCAN_FSIZE_MAX)) { 
          scan_fsize = (kk_ssize_t)kk_int_unbox(kk_block_field(b, 0)); 
          i++;  // skip the scan field itself (and the full scan_fsize does not include the field itself)
        }
        // free fields up to the last one
        for (; i < (scan_fsize-1); i++) {
          kk_box_t v = kk_block_field(b, i);
          if (kk_box_is_non_null_ptr(v)) {
            kk_block_t* vb = kk_ptr_unbox(v);
            if (kk_block_decref_no_free(vb)) {
              kk_block_drop_free_rec(vb, vb->header.scan_fsize, depth+1, ctx); // recurse with increased depth
            }
          }
        }
        // and recurse into the last one
        kk_box_t v = kk_block_field(b,scan_fsize - 1);
        kk_block_free(b);
        if (kk_box_is_non_null_ptr(v)) {
          b = kk_ptr_unbox(v);
          if (kk_block_decref_no_free(b)) {
            scan_fsize = b->header.scan_fsize;
            continue; // tailcall
          }
        }
        return;
      }
      else {
        // recursed too deep, push this block onto the todo list
        kk_block_push_delayed_drop_free(b,ctx);
        return;
      }
    }
  }
}




static kk_decl_noinline void kk_block_mark_shared_rec(kk_block_t* b, kk_ssize_t scan_fsize, const kk_ssize_t depth, kk_context_t* ctx) {
  while(true) {
    if (b->header.thread_shared) {
      // already shared
      return;
    } 
    kk_block_make_shared(b);
    if (scan_fsize == 0) {
      // nothing to scan
      return;
    }
    else if (scan_fsize == 1) {
      // if just one field, we can recursively scan without using stack space
      const kk_box_t v = kk_block_field(b, 0);
      if (kk_box_is_non_null_ptr(v)) {
        // try to mark the child now
        b = kk_ptr_unbox(v);
        scan_fsize = b->header.scan_fsize;
        continue; // tailcall
      }
      return;
    }
    else {
      // more than 1 field
      if (depth < MAX_RECURSE_DEPTH) {
        kk_ssize_t i = 0;
        if (kk_unlikely(scan_fsize >= KK_SCAN_FSIZE_MAX)) { 
          scan_fsize = (kk_ssize_t)kk_int_unbox(kk_block_field(b, 0)); 
          i++;
        }
        // mark fields up to the last one
        for (; i < (scan_fsize-1); i++) {
          kk_box_t v = kk_block_field(b, i);
          if (kk_box_is_non_null_ptr(v)) {
            kk_block_t* vb = kk_ptr_unbox(v);
            kk_block_mark_shared_rec(vb, vb->header.scan_fsize, depth+1, ctx); // recurse with increased depth
          }
        }
        // and recurse into the last one
        kk_box_t v = kk_block_field(b,scan_fsize - 1);
        if (kk_box_is_non_null_ptr(v)) {
          b = kk_ptr_unbox(v);
          scan_fsize = b->header.scan_fsize;
          continue; // tailcall          
        }
        return;
      }
      else {
        kk_assert(false);
        // TODO: recursed too deep, push this block onto the todo list
        // kk_block_push_delayed_drop_free(b,ctx);
        return;
      }
    }
  }
}



kk_decl_export void kk_block_mark_shared( kk_block_t* b, kk_context_t* ctx ) {
  if (!b->header.thread_shared) {
    kk_block_mark_shared_rec(b, b->header.scan_fsize, 0, ctx);
  }
}

kk_decl_export void kk_box_mark_shared( kk_box_t b, kk_context_t* ctx ) {
  if (kk_box_is_non_null_ptr(b)) {
    kk_block_mark_shared( kk_ptr_unbox(b), ctx );
  }
}
