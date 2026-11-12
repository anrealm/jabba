//! A minimal JSON value for request-record fields and the record itself.

use std::fmt;

/// A JSON value. Objects keep insertion order.
#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    /// Value of `key` if this is an object that has it.
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Object(items) => items.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
}

impl fmt::Display for Json {
    /// Compact JSON text.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Json::Null => f.write_str("null"),
            Json::Bool(b) => write!(f, "{b}"),
            Json::Int(i) => write!(f, "{i}"),
            // Debug keeps the fraction (1.0, not 1) and uses exponents for extremes (1e300).
            Json::Float(x) if x.is_finite() => write!(f, "{x:?}"),
            Json::Float(_) => f.write_str("null"),
            Json::Str(s) => write_str(f, s),
            Json::Array(items) => {
                f.write_str("[")?;
                for (i, item) in items.iter().enumerate() {
                    if i > 0 {
                        f.write_str(",")?;
                    }
                    write!(f, "{item}")?;
                }
                f.write_str("]")
            }
            Json::Object(items) => {
                f.write_str("{")?;
                for (i, (key, value)) in items.iter().enumerate() {
                    if i > 0 {
                        f.write_str(",")?;
                    }
                    write_str(f, key)?;
                    write!(f, ":{value}")?;
                }
                f.write_str("}")
            }
        }
    }
}

fn write_str(f: &mut fmt::Formatter<'_>, s: &str) -> fmt::Result {
    f.write_str("\"")?;
    for c in s.chars() {
        match c {
            '"' => f.write_str("\\\"")?,
            '\\' => f.write_str("\\\\")?,
            '\n' => f.write_str("\\n")?,
            '\r' => f.write_str("\\r")?,
            '\t' => f.write_str("\\t")?,
            c if (c as u32) < 0x20 => write!(f, "\\u{:04x}", c as u32)?,
            c => write!(f, "{c}")?,
        }
    }
    f.write_str("\"")
}

impl From<bool> for Json {
    fn from(v: bool) -> Self {
        Json::Bool(v)
    }
}

impl From<i64> for Json {
    fn from(v: i64) -> Self {
        Json::Int(v)
    }
}

impl From<i32> for Json {
    fn from(v: i32) -> Self {
        Json::Int(v.into())
    }
}

impl From<u32> for Json {
    fn from(v: u32) -> Self {
        Json::Int(v.into())
    }
}

impl From<u64> for Json {
    fn from(v: u64) -> Self {
        i64::try_from(v).map_or(Json::Float(v as f64), Json::Int)
    }
}

impl From<usize> for Json {
    fn from(v: usize) -> Self {
        i64::try_from(v).map_or(Json::Float(v as f64), Json::Int)
    }
}

impl From<f32> for Json {
    fn from(v: f32) -> Self {
        Json::Float(v.into())
    }
}

impl From<&String> for Json {
    fn from(v: &String) -> Self {
        Json::Str(v.clone())
    }
}

impl From<f64> for Json {
    fn from(v: f64) -> Self {
        Json::Float(v)
    }
}

impl From<&str> for Json {
    fn from(v: &str) -> Self {
        Json::Str(v.to_owned())
    }
}

impl From<String> for Json {
    fn from(v: String) -> Self {
        Json::Str(v)
    }
}

impl<T: Into<Json>> From<Option<T>> for Json {
    fn from(v: Option<T>) -> Self {
        v.map_or(Json::Null, Into::into)
    }
}

impl<T: Into<Json>> From<Vec<T>> for Json {
    fn from(v: Vec<T>) -> Self {
        Json::Array(v.into_iter().map(Into::into).collect())
    }
}

#[cfg(test)]
mod tests {
    use super::Json;

    #[test]
    fn writes_compact_json_with_escapes() {
        let value = Json::Object(vec![
            ("s".into(), Json::from("a\"b\\c\n\u{1}")),
            ("n".into(), Json::Null),
            ("f".into(), Json::Float(0.5)),
            ("nan".into(), Json::Float(f64::NAN)),
            ("a".into(), Json::from(vec![1i64, 2])),
        ]);
        assert_eq!(
            value.to_string(),
            r#"{"s":"a\"b\\c\n\u0001","n":null,"f":0.5,"nan":null,"a":[1,2]}"#
        );
    }
}
