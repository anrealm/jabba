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

impl Json {
    /// Parse JSON text (RFC 8259: no NaN or Infinity).
    pub fn parse(text: &str) -> Result<Json, String> {
        let mut p = Parser {
            s: text.as_bytes(),
            i: 0,
            depth: 0,
        };
        let v = p.value()?;
        p.ws();
        if p.i != p.s.len() {
            return Err(p.error("trailing characters"));
        }
        Ok(v)
    }
}

/// Nesting deeper than this is refused rather than risking the stack.
const MAX_DEPTH: usize = 512;

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
    depth: usize,
}

impl Parser<'_> {
    fn error(&self, what: &str) -> String {
        format!("json: {what} at offset {}", self.i)
    }

    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\n' | b'\r' | b'\t') {
            self.i += 1;
        }
    }

    fn eat(&mut self, c: u8) -> bool {
        self.ws();
        if self.s.get(self.i) == Some(&c) {
            self.i += 1;
            true
        } else {
            false
        }
    }

    fn expect(&mut self, c: u8) -> Result<(), String> {
        if self.eat(c) {
            Ok(())
        } else {
            Err(self.error(&format!("expected '{}'", c as char)))
        }
    }

    fn literal(&mut self, word: &str) -> bool {
        if self.s[self.i..].starts_with(word.as_bytes()) {
            self.i += word.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.ws();
        match self.s.get(self.i) {
            None => Err(self.error("unexpected end")),
            Some(b'{' | b'[') => {
                if self.depth == MAX_DEPTH {
                    return Err(self.error("nested too deeply"));
                }
                self.depth += 1;
                let v = if self.s[self.i] == b'{' {
                    self.object()
                } else {
                    self.array()
                };
                self.depth -= 1;
                v
            }
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(_) if self.literal("true") => Ok(Json::Bool(true)),
            Some(_) if self.literal("false") => Ok(Json::Bool(false)),
            Some(_) if self.literal("null") => Ok(Json::Null),
            Some(_) => self.number(),
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.expect(b'{')?;
        let mut items = Vec::new();
        if self.eat(b'}') {
            return Ok(Json::Object(items));
        }
        loop {
            self.ws();
            let key = self.string()?;
            self.expect(b':')?;
            items.push((key, self.value()?));
            if !self.eat(b',') {
                break;
            }
        }
        self.expect(b'}')?;
        Ok(Json::Object(items))
    }

    fn array(&mut self) -> Result<Json, String> {
        self.expect(b'[')?;
        let mut items = Vec::new();
        if self.eat(b']') {
            return Ok(Json::Array(items));
        }
        loop {
            items.push(self.value()?);
            if !self.eat(b',') {
                break;
            }
        }
        self.expect(b']')?;
        Ok(Json::Array(items))
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let digits = self
            .s
            .get(self.i..self.i + 4)
            .ok_or_else(|| self.error("bad \\u escape"))?;
        let mut v = 0;
        for &d in digits {
            let x = (d as char)
                .to_digit(16)
                .ok_or_else(|| self.error("bad \\u escape"))?;
            v = v * 16 + x;
        }
        self.i += 4;
        Ok(v)
    }

    fn string(&mut self) -> Result<String, String> {
        if self.s.get(self.i) != Some(&b'"') {
            return Err(self.error("expected a string"));
        }
        self.i += 1;
        let mut out: Vec<u8> = Vec::new();
        loop {
            let c = *self
                .s
                .get(self.i)
                .ok_or_else(|| self.error("unterminated string"))?;
            self.i += 1;
            match c {
                b'"' => break,
                b'\\' => {
                    let e = *self.s.get(self.i).ok_or_else(|| self.error("bad escape"))?;
                    self.i += 1;
                    match e {
                        b'"' => out.push(b'"'),
                        b'\\' => out.push(b'\\'),
                        b'/' => out.push(b'/'),
                        b'b' => out.push(8),
                        b'f' => out.push(12),
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xD800..0xDC00).contains(&cp)
                                && self.s[self.i..].starts_with(b"\\u")
                            {
                                let back = self.i;
                                self.i += 2;
                                let low = self.hex4()?;
                                if (0xDC00..0xE000).contains(&low) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                } else {
                                    self.i = back; // a lone high surrogate; the next escape stands
                                }
                            }
                            // lone surrogates become U+FFFD
                            let ch = char::from_u32(cp).unwrap_or('\u{FFFD}');
                            let mut buf = [0u8; 4];
                            out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                        }
                        _ => return Err(self.error("bad escape")),
                    }
                }
                0..=0x1F => return Err(self.error("control character in a string")),
                c => out.push(c),
            }
        }
        String::from_utf8(out).map_err(|_| self.error("invalid UTF-8"))
    }

    fn digits(&mut self) -> usize {
        let start = self.i;
        while self.s.get(self.i).is_some_and(u8::is_ascii_digit) {
            self.i += 1;
        }
        self.i - start
    }

    /// `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`
    fn number(&mut self) -> Result<Json, String> {
        let start = self.i;
        if self.s.get(self.i) == Some(&b'-') {
            self.i += 1;
        }
        match self.s.get(self.i) {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => return Err(self.error("unexpected character")),
        }
        let mut float = false;
        if self.s.get(self.i) == Some(&b'.') {
            self.i += 1;
            float = true;
            if self.digits() == 0 {
                return Err(self.error("bad number"));
            }
        }
        if matches!(self.s.get(self.i), Some(b'e' | b'E')) {
            self.i += 1;
            float = true;
            if matches!(self.s.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if self.digits() == 0 {
                return Err(self.error("bad number"));
            }
        }
        let text =
            std::str::from_utf8(&self.s[start..self.i]).map_err(|_| self.error("bad number"))?;
        if !float {
            if let Ok(i) = text.parse::<i64>() {
                return Ok(Json::Int(i));
            }
        }
        text.parse::<f64>()
            .ok()
            .filter(|f| f.is_finite())
            .map(Json::Float)
            .ok_or_else(|| self.error("bad number"))
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
    fn parses_what_it_writes() {
        let text = r#"{"s":"a\"b\\c\n\u0001\u00e9\ud83d\ude00","n":null,"i":-3,"f":0.5,"a":[true,false,[]],"o":{}}"#;
        let v = Json::parse(text).unwrap();
        assert_eq!(Json::parse(&v.to_string()).unwrap(), v);
        assert_eq!(v.get("s"), Some(&Json::from("a\"b\\c\n\u{1}é😀")));
        assert_eq!(v.get("i"), Some(&Json::Int(-3)));
        assert!(Json::parse("NaN").is_err());
        assert!(Json::parse("[1,]").is_err());
        assert!(Json::parse("{} x").is_err());
    }

    #[test]
    fn parses_strictly() {
        for bad in [
            "+1",
            "01",
            "1.",
            ".5",
            "-",
            "1e",
            "\"a\tb\"",
            "\"\\u+041\"",
            "\"\u{1}\"",
        ] {
            assert!(Json::parse(bad).is_err(), "{bad} was accepted");
        }
        for (good, value) in [
            ("-0", Json::Int(0)),
            ("1e3", Json::Float(1000.0)),
            ("2.5E-1", Json::Float(0.25)),
        ] {
            assert_eq!(Json::parse(good).unwrap(), value);
        }
        // a lone high surrogate becomes U+FFFD and the escape after it stands
        assert_eq!(
            Json::parse(r#""\ud83d\u0041""#).unwrap(),
            Json::from("\u{FFFD}A")
        );
        let deep = "[".repeat(100_000);
        assert!(Json::parse(&deep)
            .unwrap_err()
            .contains("nested too deeply"));
        let ok = format!("{}{}", "[".repeat(500), "]".repeat(500));
        assert!(Json::parse(&ok).is_ok());
    }

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
