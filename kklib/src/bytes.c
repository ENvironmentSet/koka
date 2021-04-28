/*---------------------------------------------------------------------------
  Copyright 2020 Daan Leijen, Microsoft Corporation.

  This is free software; you can redibibute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the file "license.txt" at the root of this dibibution.
---------------------------------------------------------------------------*/
#include "kklib.h"

/*--------------------------------------------------------------------------------------------------
  Low level allocation of bytes
--------------------------------------------------------------------------------------------------*/


// Allocate `len` bytes.
// If (p /= NULL) then initialize with at most `min(len,plen)` bytes from `p`, which must point to at least `plen` valid bytes. 
// Adds a terminating zero at the end. Return the raw buffer pointer in `buf` if non-NULL
kk_decl_export kk_decl_noinline kk_bytes_t kk_bytes_alloc_len(size_t len, size_t plen, const uint8_t* p, uint8_t** buf, kk_context_t* ctx) {
  // kk_assert_internal(s == NULL || blen(s) >= len);  // s may contain embedded 0 characters
  static uint8_t empty[16] = { 0 };
  if (len == 0) {
    if (buf != NULL) *buf = empty;
    return kk_bytes_empty();
  }
  if (plen > len) plen = len;  // limit plen <= len
  if (len <= KK_BYTES_SMALL_MAX) {
    kk_bytes_small_t b = kk_block_alloc_as(struct kk_bytes_small_s, 0, KK_TAG_BYTES_SMALL, ctx);
    b->u.buf_value = ~KUZ(0);
    if (p != NULL && plen > 0) {
      memcpy(&b->u.buf[0], p, plen);
    }
    b->u.buf[len] = 0;
    if (buf != NULL) *buf = &b->u.buf[0];
    return kk_datatype_from_base(&b->_base);
  }
  else {
    kk_bytes_normal_t b = kk_block_assert(kk_bytes_normal_t, kk_block_alloc_any(sizeof(struct kk_bytes_normal_s) - 1 /* char b[1] */ + len + 1 /* 0 terminator */, 0, KK_TAG_BYTES, ctx), KK_TAG_BYTES);
    if (p != NULL && plen > 0) {
      memcpy(&b->buf[0], p, plen);
    }
    b->length = len;
    b->buf[len] = 0;
    if (buf != NULL) *buf = &b->buf[0];
    // todo: kk_assert valid utf-8 in debug mode
    return kk_datatype_from_base(&b->_base);
  }
}


kk_bytes_t kk_bytes_adjust_length(kk_bytes_t b, size_t newlen, kk_context_t* ctx) {
  if (newlen==0) {
    kk_bytes_drop(b, ctx);
    return kk_bytes_empty();
  }
  size_t len;
  const uint8_t* s = kk_bytes_buf_borrow(b,&len);
  if (len == newlen) {
    return b;
  }
  else if (len > newlen && (3*(len/4)) < newlen &&  // 0.75*len < newlen < len: update length in place if we can
           kk_datatype_is_unique(b) && kk_datatype_has_tag(b, KK_TAG_BYTES)) {
    // length in place
    kk_assert_internal(kk_datatype_has_tag(b, KK_TAG_BYTES) && kk_datatype_is_unique(b));
    kk_bytes_normal_t nb = kk_datatype_as_assert(kk_bytes_normal_t, b, KK_TAG_BYTES);
    nb->length = newlen;
    nb->buf[newlen] = 0;
    // kk_assert_internal(kk_bytes_is_valid(kk_bytes_dup(s),ctx));
    return b;
  }
  else if (newlen < len) {
    // full copy
    kk_bytes_t tb = kk_bytes_alloc_dupn(newlen, s, ctx);
    kk_bytes_drop(b, ctx);
    return tb;
  }
  else {
    // full copy
    kk_assert_internal(newlen > len);
    uint8_t* t;
    kk_bytes_t tb = kk_bytes_alloc_buf(newlen, &t, ctx);
    memcpy( t, s, len );
    memset(t + len, 0, newlen - len);
    kk_bytes_drop(b, ctx);
    return tb;
  }
}


/*--------------------------------------------------------------------------------------------------
  Compare
--------------------------------------------------------------------------------------------------*/

static const uint8_t* kk_memmem(const uint8_t* p, size_t plen, const uint8_t* pat, size_t patlen) {
  // todo: optimize search algo?
  kk_assert(p != NULL && pat != NULL);
  if (patlen == 0 || patlen > plen) return NULL;
  const uint8_t* end = p + (plen - (patlen-1));
  for (; p < end; p++) {
    if (memcmp(p, pat, patlen) == 0) return p;
  }
  return NULL;
}

int kk_bytes_cmp_borrow(kk_bytes_t b1, kk_bytes_t b2) {
  if (kk_bytes_ptr_eq_borrow(b1, b2)) return 0;
  size_t len1;
  const uint8_t* s1 = kk_bytes_buf_borrow(b1,&len1);
  size_t len2;
  const uint8_t* s2 = kk_bytes_buf_borrow(b2,&len2);
  size_t minlen = (len1 <= len2 ? len1 : len2);
  int ord = memcmp(s1, s2, minlen);
  if (ord == 0) {
    if (len1 > len2) return 1;
    else if (len1 < len2) return -1;
  }
  return ord;
}

int kk_bytes_cmp(kk_bytes_t b1, kk_bytes_t b2, kk_context_t* ctx) {
  int ord = kk_bytes_cmp_borrow(b1,b2);
  kk_bytes_drop(b1,ctx);
  kk_bytes_drop(b2,ctx);
  return ord;
}


/*--------------------------------------------------------------------------------------------------
  Utilities
--------------------------------------------------------------------------------------------------*/

size_t kk_decl_pure kk_bytes_count_pattern_borrow(kk_bytes_t b, kk_bytes_t pattern) {
  size_t patlen;
  const uint8_t* pat = kk_bytes_buf_borrow(pattern,&patlen);  
  size_t len;
  const uint8_t* s   = kk_bytes_buf_borrow(b,&len);
  if (patlen == 0)  return kk_bytes_len_borrow(b);
  if (patlen > len) return 0;
  
  //todo: optimize by doing backward Boyer-Moore? or use forward Knuth-Morris-Pratt?
  size_t count = 0;
  const uint8_t* end = s + (len - (patlen - 1));
  for (const uint8_t* p = s; p < end; p++) {
    if (memcmp(p, pat, patlen) == 0) {
      count++;
      p += (patlen - 1);
    }    
  }
  return count;
}


kk_bytes_t kk_bytes_cat(kk_bytes_t b1, kk_bytes_t b2, kk_context_t* ctx) {
  size_t len1;
  const uint8_t* s1 = kk_bytes_buf_borrow(b1, &len1);
  size_t len2;
  const uint8_t* s2 = kk_bytes_buf_borrow(b2, &len2);
  uint8_t* p;
  kk_bytes_t t = kk_bytes_alloc_buf(len1 + len2, &p, ctx );
  memcpy(p, s1, len1);
  memcpy(p+len1, s2, len2);
  kk_assert_internal(p[len1+len2] == 0);
  kk_bytes_drop(b1, ctx);
  kk_bytes_drop(b2, ctx);
  return t;
}

kk_bytes_t kk_bytes_cat_from_buf(kk_bytes_t b1, size_t len2, const uint8_t* b2, kk_context_t* ctx) {
  if (b2 == NULL || len2 == 0) return b1;
  size_t len1;
  const uint8_t* s1 = kk_bytes_buf_borrow(b1,&len1);
  uint8_t* p;
  kk_bytes_t t = kk_bytes_alloc_buf(len1 + len2, &p, ctx);
  memcpy(p, s1, len1);
  memcpy(p+len1, b2, len2);
  kk_assert_internal(p[len1+len2] == 0);
  kk_bytes_drop(b1, ctx);
  return t;
}

kk_vector_t kk_bytes_splitv(kk_bytes_t s, kk_bytes_t sep, kk_context_t* ctx) {
  return kk_bytes_splitv_atmost(s, sep, SIZE_MAX, ctx);
}

kk_vector_t kk_bytes_splitv_atmost(kk_bytes_t b, kk_bytes_t sepb, size_t n, kk_context_t* ctx) 
{
  if (n < 1) n = 1;
  size_t len;
  const uint8_t* s = kk_bytes_buf_borrow(b, &len);
  const uint8_t* const end = s + len;
  size_t seplen;
  const uint8_t* sep = kk_bytes_buf_borrow(sepb, &seplen);

  // count parts
  size_t count = 1;
  if (seplen > 0) {    
    const uint8_t* p = s;
    while (count < n && (p = kk_memmem(p, end - p, sep, seplen)) != NULL) {
      p += seplen;
      count++;
    }
  }
  else if (n > 1) {
    count = len;
    if (count > n) count = n;
  }
  kk_assert_internal(count >= 1 && count <= n);
  
  // copy to vector
  kk_vector_t vec = kk_vector_alloc(count, kk_box_null, ctx);
  kk_box_t* v  = kk_vector_buf(vec, NULL);
  const uint8_t* p = s;
  for (size_t i = 0; i < (count-1) && p < end; i++) {
    const uint8_t* r;
    if (seplen > 0) {
      r = kk_memmem(p, end - p,  sep, seplen);
    }
    else {
      r = p + 1;
    }
    kk_assert_internal(r != NULL && r >= p && r < end);    
    const size_t partlen = (size_t)(r - p);
    v[i] = kk_bytes_box(kk_bytes_alloc_dupn(partlen, p, ctx));
    p = r + seplen;  // advance
  }
  kk_assert_internal(p <= end);
  v[count-1] = kk_bytes_box(kk_bytes_alloc_dupn(end - p, p, ctx));  // todo: share bytes if p == s ?
  kk_bytes_drop(b,ctx);
  kk_bytes_drop(sepb, ctx);
  return vec;
}

kk_bytes_t kk_bytes_replace_all(kk_bytes_t s, kk_bytes_t pat, kk_bytes_t rep, kk_context_t* ctx) {
  return kk_bytes_replace_atmost(s, pat, rep, SIZE_MAX, ctx);
}

kk_bytes_t kk_bytes_replace_atmost(kk_bytes_t s, kk_bytes_t pat, kk_bytes_t rep, size_t n, kk_context_t* ctx) {
  kk_bytes_t t = s;
  if (n==0 || kk_bytes_is_empty_borrow(s) || kk_bytes_is_empty_borrow(pat)) goto done;

  size_t plen;
  const uint8_t* p = kk_bytes_buf_borrow(s,&plen);
  size_t ppat_len;
  const uint8_t* ppat = kk_bytes_buf_borrow(pat,&ppat_len);
  size_t prep_len; 
  const uint8_t* prep = kk_bytes_buf_borrow(rep, &prep_len);
  
  const uint8_t* const pend = p + plen;
  // if unique s && |rep| == |pat|, update in-place
  // TODO: if unique s & |rep| <= |pat|, maybe update in-place if not too much waste?
  if (kk_datatype_is_unique(s) && ppat_len == prep_len) {
    size_t count = 0;
    while (count < n && p < pend) {
      const uint8_t* r = kk_memmem(p, pend - p, ppat, ppat_len);
      if (r == NULL) break;
      memcpy((uint8_t*)r, prep, prep_len);
      count++;
      p = r + prep_len;
    }
  }
  else {
    // count pat occurrences so we can pre-allocate the result buffer
    size_t count = 0;
    const uint8_t* r = p;
    while (count < n && ((r = kk_memmem(r, pend - r, ppat, ppat_len)) != NULL)) {
      count++;
      r += ppat_len;
    }
    if (count == 0) goto done; // no pattern found
    
    // allocate
    size_t newlen = plen - (count * ppat_len) + (count * prep_len);
    uint8_t* q;
    t = kk_bytes_alloc_buf(newlen, &q, ctx);
    while (count > 0) {
      count--;
      r = kk_memmem(p, pend - p, ppat, ppat_len);
      kk_assert_internal(r != NULL);
      size_t ofs = (size_t)(r - p);
      memcpy(q, p, ofs);
      memcpy(q + ofs, prep, prep_len);
      q += ofs + prep_len;
      p += ofs + ppat_len;
    }
    size_t rest = (size_t)(pend - p);
    memcpy(q, p, rest);
    kk_assert_internal(q + rest == kk_bytes_buf_borrow(t,NULL) + newlen);
  }

done:
  kk_bytes_drop(pat, ctx);
  kk_bytes_drop(rep, ctx);
  if (!kk_datatype_eq(t, s)) kk_datatype_drop(s, ctx);
  return t;
}


kk_bytes_t kk_bytes_repeat(kk_bytes_t b, size_t n, kk_context_t* ctx) {
  size_t len;
  const uint8_t* s = kk_bytes_buf_borrow(b,&len);  
  if (len == 0 || n==0) return kk_bytes_empty();  
  uint8_t* t;
  kk_bytes_t tb = kk_bytes_alloc_buf(len*n, &t, ctx); // TODO: check overflow
  if (len == 1) {
    memset(t, *s, n);
    t += n;
  }
  else {
    for (size_t i = 0; i < n; i++) {
      memcpy(t, s, len);
      t += len;
    }
  }
  kk_assert_internal(*t == 0);
  kk_bytes_drop(b,ctx);
  return tb;
}

// to avoid casting to signed, return 0 for not found, or the index+1
size_t kk_bytes_index_of1(kk_bytes_t b, kk_bytes_t sub, kk_context_t* ctx) {
  size_t slen;
  const uint8_t* s = kk_bytes_buf_borrow(b, &slen);
  size_t tlen;
  const uint8_t* t = kk_bytes_buf_borrow(sub, &tlen);  
  size_t idx;
  if (tlen == 0) {
    idx = (slen == 0 ? 0 : 1);
  }
  else if (tlen > slen) {
    idx = 0;
  }
  else {
    const uint8_t* p = kk_memmem(s, slen, t, tlen);
    idx = (p == NULL ? 0 : (size_t)(p - s) + 1);
  }
  kk_bytes_drop(b, ctx);
  kk_bytes_drop(sub, ctx);
  return idx;
}

size_t kk_bytes_last_index_of1(kk_bytes_t b, kk_bytes_t sub, kk_context_t* ctx) {
  size_t slen;
  const uint8_t* s = kk_bytes_buf_borrow(b, &slen);
  size_t tlen;
  const uint8_t* t = kk_bytes_buf_borrow(sub, &tlen);
  size_t idx;
  if (tlen == 0) {
    idx = slen;
  }
  else if (tlen > slen) {
    idx = 0;
  }
  else if (tlen == slen) {
    idx = (kk_bytes_cmp_borrow(b, sub) == 0 ? 1 : 0);
  }
  else {
    const uint8_t* p;
    for (p = s + slen - tlen; p >= s; p--) {  // todo: use reverse Boyer-Moore instead of one character at a time
      if (memcmp(p, t, tlen) == 0) break;
    }
    idx = (p >= s ? (size_t)(p - s) + 1 : 0);
  }
  kk_bytes_drop(b, ctx);
  kk_bytes_drop(sub, ctx);
  return idx;
}

bool kk_bytes_starts_with(kk_bytes_t b, kk_bytes_t pre, kk_context_t* ctx) {
  size_t slen;
  const uint8_t* s = kk_bytes_buf_borrow(b, &slen);
  size_t tlen;
  const uint8_t* t = kk_bytes_buf_borrow(pre, &tlen);
  bool starts;
  if (tlen == 0) {
    starts = (slen > 0);
  }
  else if (tlen > slen) {
    starts = false;
  }
  else {
    starts = (memcmp(s, t, tlen) == 0);
  }
  kk_bytes_drop(b, ctx);
  kk_bytes_drop(pre, ctx);
  return starts;
}

bool kk_bytes_ends_with(kk_bytes_t b, kk_bytes_t post, kk_context_t* ctx) {
  size_t slen;
  const uint8_t* s = kk_bytes_buf_borrow(b, &slen);
  size_t tlen;
  const uint8_t* t = kk_bytes_buf_borrow(post, &tlen);
  bool ends;
  if (tlen == 0) {
    ends = (slen > 0);
  }
  else if (tlen > slen) {
    ends = false;
  }
  else {
    ends = (memcmp(s + slen - tlen, t, tlen) == 0);
  }
  kk_bytes_drop(b, ctx);
  kk_bytes_drop(post, ctx);
  return ends;
}

bool kk_bytes_contains(kk_bytes_t b, kk_bytes_t sub, kk_context_t* ctx) {
  return (kk_bytes_index_of1(b, sub, ctx) > 0);
}


