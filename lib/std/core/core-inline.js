/*---------------------------------------------------------------------------
  Copyright 2012-2021, Microsoft Research, Daan Leijen.

  This is free software; you can redistribute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the LICENSE file at the root of this distribution.
---------------------------------------------------------------------------*/
var _host = "unknown"

if (typeof window !== 'undefined' && window.document) {
  _host = "browser";
}
else if (typeof importScripts !== 'undefined') {
  _host = "webworker"
}
else if (typeof process !== undefined) {
  _host = "node"
}


/*------------------------------------------------
  Number formatting
------------------------------------------------*/

function _double_show_special(d) {
  if (isNaN(d)) {
    return "nan"
  }
  else if (d === -Infinity) {
    return "-inf"
  }
  else if (d === Infinity) {
    return "inf"
  }
  else {
    return "nan"
  }
}

function _double_fix_exp(s) {
  // an exponent has at least 2 digits (following the C standard)
  return s.replace(/([eE][\+\-])(\d)$/,function(m,p1,p2){ return (p2==="0" ? "" : p1 + "0" + p2); });
}

function _double_show_exp(d,fractionDigits) {
  var s;
  if (!isFinite(d)) {
    s = _double_show_special(d);
  }
  else if (d===0.0 && Object.is(d,-0.0)) {
    s = "-0";
  }
  else if (fractionDigits < 0) {
    // use at most |fractionDigits|
    s = d.toExponential();
  }
  else {
    // use exactly |fractionDigits|.
    if (fractionDigits > 20) fractionDigits = 20;
    s = d.toExponential(fractionDigits);
  }
  return _double_fix_exp(s);
}

function _double_show_fixed(d, fractionDigits) {
  var dabs = (d < 0.0 ? -d : d);
  if (!isFinite(d)) {
    return _double_show_special(d);
  }
  else if (dabs < 1.0e-15 || dabs > 1.0e+21) {
    return _double_show_exp(d,fractionDigits);
  }
  else if (fractionDigits < 0) {
    // use at most |fractionDigits|
    var s = d.toFixed(-fractionDigits);              // show at full precision
    var cap = /^([\-\+]?\d+)(\.\d+)$/.exec(s);
    if (!cap) return _double_fix_exp(s);
    var frac = cap[2].substr(0,1 - fractionDigits);  // then cut off
    return cap[1] + frac.replace(/(?:\.|([1-9]))0+$/,"$1"); // remove trailing zeros
  }
  else {
    // use exactly fractionDigits
    if (fractionDigits > 20) fractionDigits = 20;
    return _double_fix_exp(d.toFixed(fractionDigits));
  }
}

function _string_repeat(s,n) {
  if (n<=0)  return "";
  if (n===1) return s;
  if (n===2) return s+s;
  var res = "";
  while(n > 0) {
    if (n & 1) res += s;
    n >>>= 1;
    s += s;
  }
  return res;
}

function _trimzeros(s) {
  return s.replace(/\.?0+$/,"");
}

function _gformat(x,format) {
  if (typeof x === "number" && !isFinite(x)) return _double_show_special(x);    
  var hex = /^[xX]([0-9]*)/.exec(format)
  if (hex) {
    var w = parseInt(hex[1]);
    var s = x.toString(16)
    if (format[0] == 'X') s = s.toUpperCase();
    return (s.length<w ? _string_repeat("0",w - s.length) + s : s );
  }
  var exp = /^[eE]([0-9]*)/.exec(format)
  if (exp) {
    var w = parseInt(exp[1]);
    return (w>0 && w<=20 ? x.toExponential(w) : x.toExponential());
  }
  var fix = /^[fF]([0-9]*)/.exec(format)
  if (fix) {
    var w = parseInt(fix[1]);
    return _trimzeros((w > 0 && w <= 20) ? x.toFixed(w) : x.toFixed(20));
  }
  var expfix = /^[gG]([0-9]*)/.exec(format)
  if (expfix) {
    var w = parseInt(expfix[1]);
    return (w>0&&w<=20 ? x.toPrecision(w) : x.toPrecision());
  }
  /* default */
  return x.toString();
}


/*------------------------------------------------
  Exceptions
------------------------------------------------*/
function _exn_capture_stack(exn) {
  /*
  if ("captureStackTrace" in Error) {
    Error.captureStackTrace(exn,_InfoException);  // best on Node.js
  }
  else 
  */
  {
    exn.stack = (new Error()).stack; // in browsers
  }
  if (exn.stack==null) exn.stack = "";
  // strip off leaf functions from the stack trace
  exn.stack = exn.stack.replace(/\n\s*at (exn_exception|exception|(Object\.)?throw_1|Object\.error|exn_error_pattern|Object\.error_pattern|exn_error_range|Object\._vector_at)\b.*/g,"");
}

function exn_stacktrace( exn ) {
  if (exn instanceof Error && typeof exn.stack === "string") {
    return exn.stack;
  }
  else {
    return "";
  }
}

function exn_info( exn ) {
  //console.log("exn_info: " + exn.stack);
  /*
  if (exn instanceof _InfoException && exn.info != null) {
    return exn.info;
  }
  else if (exn instanceof _InfoSystemException && exn.info != null) {
    return exn.info;
  }
  else if (exn instanceof $std_core._FinalizeException) {
    return Finalize;
  }
  else if (exn instanceof AssertionError) {
    return Assert;
  }
  else
  */
  if (exn instanceof RangeError) {
    return ExnRange;
  }
  else if (exn instanceof Error && typeof exn.code === "string" ) {
    return ExnSystem(exn.code);
  }
  else {
    return ExnError;
  }
}

function exn_message( exn ) {
  if (exn==null) {
    return "invalid error";
  }
  if (typeof exn.get_message === "function") { // for FinalizeException
    var msg = exn.get_message();
    if (typeof msg === "string") return msg;
  }
  if (typeof exn.message === "string") {
    return exn.message;
  }
  else if (typeof exn === "string") {
    return exn;
  }
  else if (typeof exn.toString === "function") {
    return exn.toString();
  }
  else {
    return "Unknown error";
  };
}

// Throw a JavaScript exception as a Koka exception
export function _throw_exception( exn ) {
  var st  = exn_stacktrace(exn);
  var exc = $std_core.Exception( exn_message(exn) + (st ? "\n" + st : ""), exn_info(exn) );
  return $std_core.throw_exn(exc);
}

export function _error_from_exception( exn ) {
  var st  = exn_stacktrace(exn);
  var exc = $std_core.Exception( exn_message(exn) + (st ? "\n" + st : ""), exn_info(exn) );
  return $std_core.$Error(exc);
}

export function _unsupported_external( msg ) {
  _throw_exception(msg);
}


/*------------------------------------------------
  32-bit integer operations
--------------------------------------------------*/

export function _int32_multiply(x,y) {
  var xhi = (x >> 16) & 0xFFFF;
  var xlo = x & 0xFFFF;
  var yhi = (y >> 16) & 0xFFFF;
  var ylo = y & 0xFFFF;
  var hi  = ((xhi * ylo) + (xlo * yhi));
  return (((hi << 16) + (xlo * ylo))|0)
}

export function _int32_rotl(x,y) {
  const shift = y & 31;
  return ((x << shift) | (x >>> (32 - shift)));
}

export function _int32_rotr(x,y) {
  const shift = y & 31;
  return ((x >>> shift) | (x << (32 - shift)));
}

const _int65 = 0x10000000000000000n;

export function _int64_shr(x,y) {
  const shift = y & 63n;
  if (shift === 0n) {
    return x;
  }
  else if (x >= 0n) {
    return (x >> shift);
  }
  else {
    const i = (x + _int65) >> shift;  // make positive (in 65 bits) and shift
    return BigInt.asIntN(64, i);
  }
}

export function _int64_sar(x,y) {
  const shift = y & 63n;
  return (x >> shift);  
}

export function _int64_shl(x,y) {
  const shift = y & 63n;
  return BigInt.asIntN(64, x << shift);  
}

export function _int64_rotl(x,y) {
  const shift = y & 63n;
  const lo = _int64_shr(x, 64n - shift);
  return BigInt.asIntN(64, (x << shift) | lo);
}

export function _int64_rotr(x,y) {
  return _int64_rotl(x, 64n - y);
}

/*------------------------------------------------
  list helpers
------------------------------------------------*/

// Create a list with from a vector in constant stack space
export function _vlist(elems,tail) {
  var xs = tail || Nil;
  if (elems!=null && elems.length>0) {
    for(var i = elems.length - 1; i >= 0; i--) {
      var elem = elems[i];
      if (elem !== undefined) xs = Cons(elem,xs);
    }
  }
  return xs;
}

// Create an array from a list with constant stack space
export function _unvlist(list) {
  var elems = [];
  while(list) {
    elems.push(list.head);
    list = list.tail;
  }
  return elems;
}

// Create a vector with a function initializer
export function _vector(n, f) {
  if (n<=0) return [];
  var a = new Array(n);
  for(var i = 0; i < n; i++) {
    a[i] = f(i);
  }
  return a;
}

// Index a vector
export function _vector_at( v, i ) {
  var j = _int_to_int32(i);
  var x = v[j];
  if (x === undefined) { exn_error_range(); }
  return x;
}


/*------------------------------------------------
  General javascript helpers
------------------------------------------------*/
// make a shallow copy
function _shallow_copy(obj) {
  if (typeof obj !== 'object') return obj;
  var value = obj.valueOf();
  if (obj != value) return new obj.constructor(value);
  var newobj = {};
  for( var prop in obj) newobj[prop] = obj[prop];
  return newobj;
}

// get the fields of an object
function _fields(obj) {
  var props = [];
  for (var prop in obj) props.push(prop);
  return props;
}

// Export module `mod` extended with `exp`. Modifies `exp` in place and assigns to mod
function _export(mod,exp) {
  for(var prop in mod) {
    if (exp[prop] === undefined) {
      exp[prop] = mod[prop];
    }
  }
  return exp;
}

/* assign here so inlined primitives are available in system.core itself */
const $std_core = {"_throw_exception": _throw_exception
            , "_error_from_exception": _error_from_exception
            , "_unsupported_external": _unsupported_external
            // primitive operations emitted by the compiler
            , "_int32_multiply": _int32_multiply                      
            , "vlist": _vlist
            , "_vector_at": _vector_at
            // integer operations that will be inlined
            , "_int_string": _int_string
            , "_int_const": _int_const
            , "_int_double": _int_double
            , "_int_clamp32": _int_clamp32
            , "_int_clamp64": _int_clamp64
            , "_int_clamp_byte": _int_clamp_byte
            , "_int_from_int32": _int_from_int32
            , "_int_from_int64": _int_from_int64
            , "_double_to_int32": _double_to_int32
            , "_double_round": _double_round
            , "_int_to_double": _int_to_double
            , "_int_iszero": _int_iszero
            , "_int_isodd": _int_isodd
            , "_int_negate": _int_negate
            , "_int_abs": _int_abs
            , "_int_sign": _int_sign
            , "_int_add": _int_add
            , "_int_sub": _int_sub
            , "_int_mul": _int_mul
            , "_int_div": _int_div
            , "_int_mod": _int_mod
            , "_int_divmod": _int_divmod
            , "_int_compare": _int_compare
            , "_int_eq": _int_eq
            , "_int_ne": _int_ne
            , "_int_gt": _int_gt
            , "_int_ge": _int_ge
            , "_int_lt": _int_lt
            , "_int_le": _int_le
            };


/*------------------------------------------------
  double arithmetic
------------------------------------------------*/


export var _double_trunc = Math.trunc || function(x){ return x - x%1; };

// Round a double with rounding to even on a tie.
export function _double_round(d) {
  return Math.round(d); // rounds to +Infinity on a tie
  //return (n - d == 0.5 && n % 2 != 0 ? n - 1 : n);  // if it was a tie, and n is odd, decrement by 1
}

/*------------------------------------------------
  integer arithmetic
------------------------------------------------*/


// We represent integers as a regular number as long as it is within _min_precise and _max_precise.
// Outside that we use Integer objects.
// An Integer is just a wrapper around a BigInt; we use that so we can do faster basic
// operations. For example, for multiplication, we just multiply directly as `x * y`, and then check
// afterwards if it succeeded; if one of `x` or `y` was an `Integer` it will have been 
// cast to `NaN` (or something like `[object Object]1`) and we can fall back to big integer arithmetic.
// This is expensive if big integers dominate but we expect the vast majority of operations to work
// on "small" integers. (Note that we cannot use BigInt directly as `x * y` in such case could lead
// to type error exceptions.)
const _max_precise = 9007199254740991; // 2^53 -1
const _min_precise = -_max_precise;

const _max_int32 =  0x7FFFFFFF;
const _min_int32 = -0x80000000;

const _max_int64 =  0x7FFFFFFFFFFFFFFFn;
const _min_int64 = -0x8000000000000000n;

// is a number small?
function _is_small(x) {
  return (x >= _min_precise && x <= _max_precise);
}

const Integer = (function(){
  function Integer(x) {
    if (x instanceof Integer) {
      this.value = x.value;
    }
    else {
      this.value = BigInt(x);
    }
  }
  Integer.prototype.to_number = function() {
    const x = Number(this.value);
    return (isNaN(x) ? 0 : x);
  }  
  Integer.prototype.toString = function(radix) {
    return this.value.toString(radix);
  }
  Integer.prototype.valueOf = function() {
    return NaN; // important for optimized functions
  }

  return Integer;
})();

export function _int(x) {
  return (_is_small(x) ? Number(x) : new Integer(x));
}

export function _big(x) {
  return (x instanceof Integer ? x.value : BigInt(x));  
}

function _integer_add(x,y) {
  return _int( _big(x) + _big(y) );
}
function _integer_sub(x,y) {
  return _int( _big(x) - _big(y) );
}
function _integer_mul(x,y) {
  return _int( _big(x) * _big(y) );
}
function _integer_pow(x,y) {
  return _int( _big(x) ** _big(y) );
}
function _integer_cdiv(x,y) {
  return _int( _big(x) / _big(y) );
}
function _integer_cmod(x,y) {
  return _int( _big(x) % _big(y) );
}
function _integer_cdivmod(x,y) {
  const i = _big(x);
  const j = _big(y);
  return $std_core._Tuple2_( _int(i/j), _int(i%j) );
}
function _integer_div(x,y) {
  const i = _big(x);
  const j = _big(y);
  const q = i / j;
  const r = i % j;
  return _int( r < 0n ? (j > 0n ? q - 1n : q + 1n) : q );    
}
function _integer_mod(x,y) {
  const i = _big(x);
  const j = _big(y);
  const r = i % j;
  return _int( r < 0n ? (j > 0n ? r + j : r - j) : r );    
}
function _integer_divmod(x,y) {
  const i = _big(x);
  const j = _big(y);
  var q = i / j;
  var r = i % j;
  if (r < 0n) {
    if (j > 0n) { q--; r += j; }
           else { q++; r -= j; }
  }
  return $std_core._Tuple2_( _int(q), _int(r) );
}
function _integer_neg(x) {
  return _int( 0n - _big(x));
}
function _integer_inc(x) {
  return _int( _big(x) + 1n );
}
function _integer_dec(x) {
  return _int( _big(x) - 1n );
}
function _integer_abs(x) {
  const i = _big(x);
  return _int( i >= 0n ? i : 0n - i );
}
function _integer_compare(x,y) {
  const i = _big(x);
  const j = _big(y);
  return (i === j ? $std_core_types.Eq : (i > j ? $std_core_types.Gt : $std_core_types.Lt));
}
function _integer_lt(x,y) {
  return (_big(x) < _big(y));
}
function _integer_lte(x,y) {
  return (_big(x) <= _big(y));
}
function _integer_gt(x,y) {
  return (_big(x) > _big(y));
}
function _integer_gte(x,y) {
  return (_big(x) >= _big(y));
}
function _integer_eq(x,y) {
  return (_big(x) === _big(y));
}
function _integer_neq(x,y) {
  return (_big(x) !== _big(y));
}
function _integer_mul_pow10(x,n) {
  return _int( _big(x) * (10n ** _big(n)) );
}
function _integer_div_pow10(x,n) {
  return _integer_div( x, _int( 10n ** _big(n) ) );
}
function _integer_cdiv_pow10(x,n) {
  return _integer_cdiv( x, _int( 10n ** _big(n) ) );
}
function _integer_count_pow10(x) {
  const zeros = _big(x).toString().match(/(0+)n$/);
  return (zeros == null || zeros.length <= 1 ? 0 : zeros[1].length);
}
function _integer_count_digits(x) {
  const i = _big(x);
  const s = (i >= 0n ? i : 0n - i).toString();
  return (s.length - 1);
}
function _integer_is_odd(x) {
  const i = _big(x);
  if (_is_small(i)) {
    return ((Number(i) & 1) === 1 );
  }
  else {
    return ((i % 2n) === 1n);
  }
}

export function _int_add(x,y) {
  const z = x + y;
  return (_is_small(z) ? z : _integer_add(x,y));
}

export function _int_sub(x,y) {
  const z = x - y;
  return (_is_small(z) ? z : _integer_sub(x,y));
}

export function _int_mul(x,y) {
  const z = x * y;
  return (_is_small(z) ? z : _integer_mul(x,y));
}

export function _int_iszero(x) {
  return (x instanceof Integer ? x.value === 0n : x===0);
}

export function _int_isodd(x) {
  return (x instanceof Integer ? _integer_is_odd(x) : (x&1)===1);
}


export function _int_negate(x) {
  const z = 0 - x;
  return (_is_small(z) ? z : _integer_neg(x));
}

export function _int_abs(x) {
  return (x instanceof Integer ? _integer_abs(x) : Math.abs(x) );
}

export function _int_cdivmod(x,y) {
  const q = _double_trunc(x / y);
  if (!isNaN(q)) {
    return $std_core_types._Tuple2_(q,(x%y));
  }
  else {
    return _integer_cdivmod(x,y);
  }
}

export function _int_cdiv(x,y) {
  const q = _double_trunc(x / y);
  return (!isNaN(q) ? q : _integer_cdiv(x,y));
}

export function _int_cmod(x,y) {
  const r = (x % y);
  return (!isNaN(r) ? r : _integer_cmod(x,y));
}

export function _int_divmod(x,y) {
  if (_int_iszero(y)) return 0;
  var q = _double_trunc(x / y);
  if (!isNaN(q)) {
    var r = x%y;
    if (r<0) {
      if (y>0) { q--; r += y; }
          else { q++; r -= y; }
    }
    return $std_core_types._Tuple2_(q,r);
  }
  else {
    return _integer_divmod(x,y)
  }
}

export function _int_div(x,y) {
  if (_int_iszero(y)) return 0;
  const q = _double_trunc(x/y);
  if (!isNaN(q)) {
    const r = (x%y);
    return (r<0 ? (y>0 ? q-1 : q+1) : q);
  }
  else return _integer_div(x,y);
}

export function _int_mod(x,y) {
  if (_int_iszero(y)) return 0;
  const r = (x%y);
  if (!isNaN(r)) {
    return (r<0 ? (y>0 ? r+y : r-y) : r);
  }
  else return _integer_mod(x,y);
}


export function _int_compare(x,y) {
  const d = x - y;
  if (!isNaN(d)) {
    return (d>0 ? $std_core_types.Gt : (d<0 ? $std_core_types.Lt : $std_core_types.Eq));
  }
  else {
    return _integer_compare(x,y);
  }
}

export function _int_sign(x) {
  return _int_compare(x,0);
}

export function _int_eq(x,y)   { 
  const d = x - y;
  if (!isNaN(d)) {
    return (d === 0);
  }
  else {
    return _integer_eq(x,y);
  }
}

export function _int_ne(x,y)   { 
  const d = x - y;
  if (!isNaN(d)) {
    return (d !== 0);
  }
  else {
    return _integer_neq(x,y);
  }
}

export function _int_lt(x,y) { 
  const d = x - y;
  if (!isNaN(d)) {
    return (d < 0);
  }
  else {
    return _integer_lt(x,y);
  }
}

export function _int_le(x,y) { 
  const d = x - y;
  if (!isNaN(d)) {
    return (d <= 0);
  }
  else {
    return _integer_lte(x,y);
  }
}

export function _int_gt(x,y) { 
  const d = x - y;
  if (!isNaN(d)) {
    return (d > 0);
  }
  else {
    return _integer_gt(x,y);
  }
}

export function _int_ge(x,y) { 
  const d = x - y;
  if (!isNaN(d)) {
    return (d >= 0);
  }
  else {
    return _integer_gte(x,y);
  }
}

export function _int_pow(i,exp) {
	if (_is_small(i)) {
		var j = Math.pow(i);
		if (_is_small(j)) return j;
	}
	return _integer_pow(i,exp);
}


export function _int_mul_pow10(i,n) {
  const s = _int_sign(n);
  if (s === 0) return i;
  if (s < 0) return _int_cdiv_pow10(i, _int_negate(n) );
  return (_is_small(i) && n <= 14 ? _int_mul(i,Math.pow(10,n)) : _integer_mul_pow10(i,n) );
}

export function _int_cdiv_pow10(i,n) {
  const s = _int_sign(n);
  if (s === 0) return i;
  if (s < 0) return _int_mul_pow10(i, _int_negate(n) );  
  return (_is_small(i) && n <= 14 ? _int_cdiv(i,Math.pow(10,n)) : _integer_cdiv_pow10(i,n) );
}


function _count_pow10( x ) {
  var j = 0;
  while(x!==0) {
    var m = x%10;
    if (m===0) { j++; }
          else break;
    x = x/10;
  }
  return j;
}
export function _int_count_pow10(i) {
  return (_is_small(i) ? _count_pow10(i) : _integer_count_pow10(i) );
}


function _count_digits8( x ) {  // only for -1e8 < x < 1e8
  if (x===0) return 0;
  x = Math.abs(x)
  if (x < 1e4) { // 1 - 4
    if (x < 1e2) return (x < 10 ? 1 : 2);
            else return (x < 1000 ? 3 : 4);
  }
  else { // 5 - 8
    if (x < 1e6) return (x < 1e5 ? 5 : 6);
            else return (x < 1e7 ? 7 : 8);
  }
}
export function _int_count_digits(i) {
  return (i > -1e8 && i < 1e8 ? _count_digits8(i) : _integer_count_digits(i) );
}


// create an int from a string.
export function _int_string(s) {
  if (s.length < 15) return parseInt(s);
                else return _int( BigInt(s) );
}

// create an int from a big int
export function _int_const(i) {
  return _int(i);
}

// create an int from a double.
export function _int_double(x) {
  if (_is_small(x)) return _double_round(x);
  if (isFinite(x)) return new Integer(x);
  if (x===Infinity) return _max_int32;
  if (x===-Infinity) return _min_int32;
  return 0;
}

function _int_to_number(x) {
  return (x instanceof Integer ? x.to_number() : x);
}

// Clamp a big integer into a 32 bit integer range.
export function _int_clamp32(x) {
  const v = _int_to_number(x);
  if (v > _max_int32) return _max_int32;
  if (v < _min_int32) return _min_int32;
  return (v|0);
}

export function _int_from_int32(x) {
  return x;
}

export function _int_clamp64(x) {
  if (_is_small(x)) return BigInt(x);
  const v = _big(x);
  if (v > _max_int64) return _max_int64;
  if (v < _min_int64) return _min_int64;
  return v;
}

export function _int_from_int64(x) {
  return _int(x);
}

export function _int_clamp_byte(x) {
  const v = _int_to_number(x);
  if (v > 255) return 255;
  if (v < 0) return 0;
  return (v|0);
}

// Clamp a double into a 32 bit integer range.
export function _double_to_int32(x) {
  if (x > _max_int32) return _max_int32;
  if (x < _min_int32) return _min_int32;
  if (isNaN(x)) return 0;
  return (x|0);
}

export function _int_to_double(x) {
  return (x instanceof Integer ? x.to_number() : x);
}

function _int_showhex(x,upper) {
  const s = x.toString(16);
  return (upper ? s.toUpperCase() : s);
}

function _int_parse(s,hex) {
  if (s==="") return $std_core_types.Nothing;
  const cappre  = /^([\-\+])?(0[xX])?(.*)$/.exec(s);
  const sdigits = cappre[3].toLowerCase();
  const sign    = cappre[1] || "";
  if (cappre[2]) hex = true;
  if (hex) {
    const cap = /^[0-9a-f]+$/.exec(sdigits);
    if (!cap) return  $std_core_types.Nothing;
    return $std_core_types.Just( _int_string(sign + "0x" + sdigits) );
  }
  else {
    const rx = /^([0-9]+)(?:\.([0-9]+))?(?:[eE]\+?([0-9]+))?$/;
    const cap = rx.exec(sdigits);
    if (!cap) return $std_core_types.Nothing;
    var sig  = cap[1];
    const frac = cap[2] || "";
    var exp  = (cap[3] ? parseInt(cap[3]) : 0);
    if (frac.length > exp) return $std_core_types.Nothing;
    exp = exp - frac.length;
    sig = sig + frac;
    var x = _int_string(sign + sig);
    if (exp > 0) {
      x = _int_mul_pow10( x, exp );
    }
    return $std_core_types.Just(x);
  }
}
