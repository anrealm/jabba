//! The C types of the ABI (ffi/jabba_ffi.h), mirrored with `#[repr(C)]`.
#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_void};

pub const JABBA_FFI_ABI: u32 = 1;

pub const JABBA_NULL: u32 = 0;
pub const JABBA_BOOL: u32 = 1;
pub const JABBA_INT: u32 = 2;
pub const JABBA_FLOAT: u32 = 3;
pub const JABBA_STR: u32 = 4;
pub const JABBA_BYTES: u32 = 5;
pub const JABBA_JSON: u32 = 6;
pub const JABBA_F64S: u32 = 7;
pub const JABBA_F32S: u32 = 8;
pub const JABBA_I64S: u32 = 9;
pub const JABBA_KIND_NULLABLE: u32 = 0x100;
pub const JABBA_KIND_MASK: u32 = 0xFF;

pub const JABBA_OK: u32 = 0;
pub const JABBA_FAILED: u32 = 1;
pub const JABBA_CANCELLED: u32 = 2;

pub type ReleaseFn = unsafe extern "C" fn(owner: *mut c_void);
pub type TaskFn = unsafe extern "C" fn(arg: *mut c_void);
pub type PostFn = unsafe extern "C" fn(host: *mut c_void, f: Option<TaskFn>, arg: *mut c_void);
pub type SetFieldFn = unsafe extern "C" fn(
    host: *mut c_void,
    key: *const c_char,
    key_len: usize,
    value: *const jabba_value,
);
pub type CompleteFn = unsafe extern "C" fn(call: *mut jabba_call, result: *const jabba_result);
pub type CallFn = unsafe extern "C" fn(node: *mut c_void, call: *mut jabba_call);
pub type NodeReleaseFn = unsafe extern "C" fn(node: *mut c_void);
pub type Factory = unsafe extern "C" fn() -> *const jabba_node;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct jabba_buf {
    pub data: *const c_void,
    pub len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union jabba_value_as {
    pub i: i64,
    pub f: f64,
    pub buf: jabba_buf,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct jabba_value {
    pub kind: u32,
    pub as_: jabba_value_as,
    pub release: Option<ReleaseFn>,
    pub owner: *mut c_void,
}

#[repr(C)]
pub struct jabba_host {
    pub size: u32,
    pub self_: *mut c_void,
    pub post: Option<PostFn>,
    pub set_field: Option<SetFieldFn>,
    pub request_id: *const c_char,
    pub request_id_len: usize,
}

#[repr(C)]
pub struct jabba_result {
    pub size: u32,
    pub status: u32,
    pub value: jabba_value,
    pub error: *const c_char,
    pub error_len: usize,
}

#[repr(C)]
pub struct jabba_call {
    pub size: u32,
    pub inputs: *const jabba_value,
    pub n_inputs: usize,
    pub host: *const jabba_host,
    pub complete: Option<CompleteFn>,
    pub host_data: *mut c_void,
    pub node_data: *mut c_void,
}

#[repr(C)]
pub struct jabba_node {
    pub abi: u32,
    pub size: u32,
    pub self_: *mut c_void,
    pub name: *const c_char,
    pub output_kind: u32,
    pub n_inputs: usize,
    pub input_kinds: *const u32,
    pub start: Option<CallFn>,
    pub cancel: Option<CallFn>,
    pub end: Option<CallFn>,
    pub release: Option<NodeReleaseFn>,
}

impl jabba_value {
    pub fn null() -> Self {
        Self::scalar(JABBA_NULL)
    }

    pub fn scalar(kind: u32) -> Self {
        jabba_value {
            kind,
            as_: jabba_value_as { i: 0 },
            release: None,
            owner: std::ptr::null_mut(),
        }
    }

    pub fn buffer(kind: u32, data: *const c_void, len: usize) -> Self {
        jabba_value {
            kind,
            as_: jabba_value_as {
                buf: jabba_buf { data, len },
            },
            release: None,
            owner: std::ptr::null_mut(),
        }
    }

    pub fn base(&self) -> u32 {
        self.kind & JABBA_KIND_MASK
    }

    /// Release it if it owns memory (and forget the owner).
    ///
    /// # Safety
    /// `release`/`owner` must be what its producer handed over, released at most once.
    pub unsafe fn release(&mut self) {
        if let Some(release) = self.release.take() {
            release(self.owner);
        }
    }
}

// The ABI's pointers are plain data; the protocol (spec/ffi.md) says who may use them when.
unsafe impl Send for jabba_value {}
unsafe impl Sync for jabba_value {}

/// `len` items at `p`: any `p` for none (C passes NULL for an empty array), `None` for a
/// NULL `p` with items.
pub(crate) unsafe fn items<'a, T>(p: *const T, len: usize) -> Option<&'a [T]> {
    if len == 0 {
        Some(&[])
    } else if p.is_null() {
        None
    } else {
        Some(std::slice::from_raw_parts(p, len))
    }
}

pub fn kind_name(kind: u32) -> String {
    const NAMES: [&str; 10] = [
        "null", "bool", "int", "float", "str", "bytes", "json", "f64s", "f32s", "i64s",
    ];
    let base = (kind & JABBA_KIND_MASK) as usize;
    let name = NAMES
        .get(base)
        .map_or_else(|| format!("kind {base}"), |n| (*n).to_owned());
    if kind & JABBA_KIND_NULLABLE != 0 {
        name + "?"
    } else {
        name
    }
}

/// Is a value of kind `actual` acceptable where `declared` is expected?
pub fn value_fits(actual: u32, declared: u32) -> bool {
    if actual & !JABBA_KIND_MASK != 0 {
        return false; // a value has a concrete kind: no nullable bit, no stray bits
    }
    let base = actual;
    if base == JABBA_NULL {
        return declared & JABBA_KIND_NULLABLE != 0 || declared & JABBA_KIND_MASK == JABBA_NULL;
    }
    base == declared & JABBA_KIND_MASK
}

/// Can a producer declaring `from` feed a consumer declaring `to`?
pub fn kind_feeds(from: u32, to: u32) -> bool {
    from & JABBA_KIND_MASK == to & JABBA_KIND_MASK
        && (from & JABBA_KIND_NULLABLE == 0 || to & JABBA_KIND_NULLABLE != 0)
}

pub fn known_kind(kind: u32) -> bool {
    kind & JABBA_KIND_MASK <= JABBA_I64S && kind & !(JABBA_KIND_MASK | JABBA_KIND_NULLABLE) == 0
}
