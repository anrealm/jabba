//! How Rust values cross the ABI.

use std::ffi::c_void;
use std::sync::Arc;

use super::sys::*;
use crate::json::Json;

/// Temporaries a borrowed input points into (JSON text, ...); kept until the call ends.
#[derive(Default)]
pub struct Scratch {
    strings: Vec<String>,
}

impl Scratch {
    fn keep(&mut self, s: String) -> &String {
        self.strings.push(s);
        self.strings.last().expect("just pushed")
    }
}

/// A Rust type that crosses the ABI: `bool`, `i64`, `f64`, `String`, `Vec<u8>` (bytes),
/// [`Json`], `Vec<f64|f32|i64>`, [`Array`] (zero-copy results), `()` (null), `Option<T>`.
pub trait FfiValue: Sized + Send + Sync + 'static {
    /// The declared kind.
    const KIND: u32;
    /// A value borrowing `self` (and `scratch`), for an input.
    fn view(&self, scratch: &mut Scratch) -> jabba_value;
    /// An owned value, for a result: its receiver releases it.
    fn into_value(self) -> jabba_value;
    /// Decode a borrowed value (copies).
    ///
    /// # Safety
    /// `v` must be a valid value of a kind that fits [`Self::KIND`].
    unsafe fn read(v: &jabba_value) -> Result<Self, String>;
    /// Decode a value and release it; zero-copy where the type allows.
    ///
    /// # Safety
    /// As for `read`; `v` is owned and released here.
    unsafe fn take(mut v: jabba_value) -> Result<Self, String> {
        let out = Self::read(&v);
        v.release();
        out
    }
}

unsafe extern "C" fn release_box<H>(owner: *mut c_void) {
    drop(Box::from_raw(owner.cast::<H>()));
}

/// An owned value whose memory lives in `holder`.
fn owned<H: Send + 'static>(
    kind: u32,
    holder: Box<H>,
    data: *const c_void,
    len: usize,
) -> jabba_value {
    let mut v = jabba_value::buffer(kind, data, len);
    v.owner = Box::into_raw(holder).cast();
    v.release = Some(release_box::<H>);
    v
}

unsafe fn bytes<'a>(v: &jabba_value) -> Result<&'a [u8], String> {
    let buf = v.as_.buf;
    if buf.len == 0 {
        return Ok(&[]);
    }
    if buf.data.is_null() {
        return Err(format!(
            "{} of length {} without data",
            kind_name(v.kind),
            buf.len
        ));
    }
    Ok(std::slice::from_raw_parts(buf.data.cast::<u8>(), buf.len))
}

/// Elements of a numeric array, copied (also when the producer misaligned them).
unsafe fn elements<E: Copy>(v: &jabba_value) -> Result<Vec<E>, String> {
    let buf = v.as_.buf;
    if buf.len == 0 {
        return Ok(Vec::new());
    }
    if buf.data.is_null() {
        return Err(format!(
            "{} of length {} without data",
            kind_name(v.kind),
            buf.len
        ));
    }
    let p = buf.data.cast::<E>();
    if p as usize % std::mem::align_of::<E>() == 0 {
        Ok(std::slice::from_raw_parts(p, buf.len).to_vec())
    } else {
        Ok((0..buf.len).map(|i| p.add(i).read_unaligned()).collect())
    }
}

impl FfiValue for bool {
    const KIND: u32 = JABBA_BOOL;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        self.into_value()
    }
    fn into_value(self) -> jabba_value {
        let mut v = jabba_value::scalar(JABBA_BOOL);
        v.as_.i = i64::from(self);
        v
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        Ok(v.as_.i != 0)
    }
}

impl FfiValue for i64 {
    const KIND: u32 = JABBA_INT;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        self.into_value()
    }
    fn into_value(self) -> jabba_value {
        let mut v = jabba_value::scalar(JABBA_INT);
        v.as_.i = self;
        v
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        Ok(v.as_.i)
    }
}

impl FfiValue for f64 {
    const KIND: u32 = JABBA_FLOAT;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        self.into_value()
    }
    fn into_value(self) -> jabba_value {
        let mut v = jabba_value::scalar(JABBA_FLOAT);
        v.as_.f = self;
        v
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        Ok(v.as_.f)
    }
}

impl FfiValue for String {
    const KIND: u32 = JABBA_STR;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        jabba_value::buffer(JABBA_STR, self.as_ptr().cast(), self.len())
    }
    fn into_value(self) -> jabba_value {
        let holder = Box::new(self);
        let (data, len) = (holder.as_ptr().cast(), holder.len());
        owned(JABBA_STR, holder, data, len)
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        String::from_utf8(bytes(v)?.to_vec()).map_err(|e| format!("str is not UTF-8: {e}"))
    }
}

impl FfiValue for Vec<u8> {
    const KIND: u32 = JABBA_BYTES;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        jabba_value::buffer(JABBA_BYTES, self.as_ptr().cast(), self.len())
    }
    fn into_value(self) -> jabba_value {
        let holder = Box::new(self);
        let (data, len) = (holder.as_ptr().cast(), holder.len());
        owned(JABBA_BYTES, holder, data, len)
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        Ok(bytes(v)?.to_vec())
    }
}

impl FfiValue for Json {
    const KIND: u32 = JABBA_JSON;
    fn view(&self, scratch: &mut Scratch) -> jabba_value {
        let text = scratch.keep(self.to_string());
        jabba_value::buffer(JABBA_JSON, text.as_ptr().cast(), text.len())
    }
    fn into_value(self) -> jabba_value {
        self.to_string().into_value_as(JABBA_JSON)
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        let text = std::str::from_utf8(bytes(v)?).map_err(|e| format!("json is not UTF-8: {e}"))?;
        Json::parse(text)
    }
}

trait IntoValueAs {
    fn into_value_as(self, kind: u32) -> jabba_value;
}

impl IntoValueAs for String {
    fn into_value_as(self, kind: u32) -> jabba_value {
        let mut v = self.into_value();
        v.kind = kind;
        v
    }
}

/// Element types of numeric arrays.
pub trait Element: Copy + Send + Sync + 'static {
    const KIND: u32;
}
impl Element for f64 {
    const KIND: u32 = JABBA_F64S;
}
impl Element for f32 {
    const KIND: u32 = JABBA_F32S;
}
impl Element for i64 {
    const KIND: u32 = JABBA_I64S;
}

impl<E: Element> FfiValue for Vec<E> {
    const KIND: u32 = E::KIND;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        jabba_value::buffer(E::KIND, self.as_ptr().cast(), self.len())
    }
    fn into_value(self) -> jabba_value {
        let holder = Box::new(self);
        let (data, len) = (holder.as_ptr().cast(), holder.len());
        owned(E::KIND, holder, data, len)
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        elements(v)
    }
}

struct ForeignMemory {
    release: ReleaseFn,
    owner: *mut c_void,
}

unsafe impl Send for ForeignMemory {}
unsafe impl Sync for ForeignMemory {}

impl Drop for ForeignMemory {
    fn drop(&mut self) {
        unsafe { (self.release)(self.owner) }
    }
}

/// A numeric array received over the ABI without copying: it keeps the producer's memory
/// until the last clone is gone. Dereferences to a slice.
#[derive(Clone)]
pub struct Array<E: Element> {
    data: *const E,
    len: usize,
    _keep: Option<Arc<ForeignMemory>>,
    _owned: Option<Arc<[E]>>,
}

unsafe impl<E: Element> Send for Array<E> {}
unsafe impl<E: Element> Sync for Array<E> {}

impl<E: Element> From<Vec<E>> for Array<E> {
    fn from(v: Vec<E>) -> Self {
        let owned: Arc<[E]> = v.into();
        Array {
            data: owned.as_ptr(),
            len: owned.len(),
            _keep: None,
            _owned: Some(owned),
        }
    }
}

impl<E: Element> std::ops::Deref for Array<E> {
    type Target = [E];
    fn deref(&self) -> &[E] {
        if self.len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.data, self.len) }
        }
    }
}

impl<E: Element + std::fmt::Debug> std::fmt::Debug for Array<E> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

impl<E: Element> FfiValue for Array<E> {
    const KIND: u32 = E::KIND;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        jabba_value::buffer(E::KIND, self.data.cast(), self.len)
    }
    fn into_value(self) -> jabba_value {
        let (data, len) = (self.data.cast(), self.len);
        owned(E::KIND, Box::new(self), data, len)
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        Ok(Array::from(elements::<E>(v)?))
    }
    unsafe fn take(mut v: jabba_value) -> Result<Self, String> {
        let buf = v.as_.buf;
        let aligned = buf.data as usize % std::mem::align_of::<E>() == 0;
        if buf.len == 0 || !aligned || v.release.is_none() {
            let out = Self::read(&v);
            v.release();
            return out;
        }
        if buf.data.is_null() {
            v.release();
            return Err(format!(
                "{} of length {} without data",
                kind_name(v.kind),
                buf.len
            ));
        }
        let keep = Arc::new(ForeignMemory {
            release: v.release.take().expect("checked"),
            owner: v.owner,
        });
        Ok(Array {
            data: buf.data.cast(),
            len: buf.len,
            _keep: Some(keep),
            _owned: None,
        })
    }
}

impl FfiValue for () {
    const KIND: u32 = JABBA_NULL;
    fn view(&self, _: &mut Scratch) -> jabba_value {
        jabba_value::null()
    }
    fn into_value(self) -> jabba_value {
        jabba_value::null()
    }
    unsafe fn read(_: &jabba_value) -> Result<Self, String> {
        Ok(())
    }
}

impl<T: FfiValue> FfiValue for Option<T> {
    const KIND: u32 = T::KIND | JABBA_KIND_NULLABLE;
    fn view(&self, scratch: &mut Scratch) -> jabba_value {
        match self {
            Some(v) => v.view(scratch),
            None => jabba_value::null(),
        }
    }
    fn into_value(self) -> jabba_value {
        self.map_or_else(jabba_value::null, FfiValue::into_value)
    }
    unsafe fn read(v: &jabba_value) -> Result<Self, String> {
        if v.base() == JABBA_NULL {
            Ok(None)
        } else {
            T::read(v).map(Some)
        }
    }
    unsafe fn take(mut v: jabba_value) -> Result<Self, String> {
        if v.base() == JABBA_NULL {
            v.release();
            Ok(None)
        } else {
            T::take(v).map(Some)
        }
    }
}

/// A borrowed value of any kind as JSON (what `set_field` writes into the record).
pub(crate) unsafe fn to_json(v: &jabba_value) -> Result<Json, String> {
    Ok(match v.base() {
        JABBA_NULL => Json::Null,
        JABBA_BOOL => Json::Bool(v.as_.i != 0),
        JABBA_INT => Json::Int(v.as_.i),
        JABBA_FLOAT => Json::Float(v.as_.f),
        JABBA_STR => Json::Str(String::read(v)?),
        JABBA_JSON => Json::read(v)?,
        JABBA_BYTES => Json::Array(bytes(v)?.iter().map(|b| Json::Int(i64::from(*b))).collect()),
        JABBA_F64S => Json::Array(elements::<f64>(v)?.into_iter().map(Json::Float).collect()),
        JABBA_F32S => Json::Array(
            elements::<f32>(v)?
                .into_iter()
                .map(|x| Json::Float(f64::from(x)))
                .collect(),
        ),
        JABBA_I64S => Json::Array(elements::<i64>(v)?.into_iter().map(Json::Int).collect()),
        other => return Err(format!("unknown kind {other}")),
    })
}
