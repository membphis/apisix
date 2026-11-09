// SPDX-License-Identifier: Apache-2.0
//! URI policy helpers ported from `module/http.c` (`ac_decode`,
//! `ac_only_decoded_or_merged`). The caller supplies the scratch buffer so
//! the module can take it from the request pool, as the C implementation does.

#[inline]
fn hex(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

/// Decodes every valid `%XX` escape of `path` into `out` (`ac_decode` with
/// `keep_slash = 0`). Returns the decoded length, or `None` when `out` is
/// shorter than `path`.
pub fn decode_into(path: &[u8], out: &mut [u8]) -> Option<usize> {
    if out.len() < path.len() {
        return None;
    }
    let (mut read, mut written) = (0, 0);
    while read < path.len() {
        let byte = path[read];
        if byte == b'%' && read + 2 < path.len() {
            if let (Some(high), Some(low)) = (hex(path[read + 1]), hex(path[read + 2])) {
                out[written] = high * 16 + low;
                written += 1;
                read += 3;
                continue;
            }
        }
        out[written] = byte;
        written += 1;
        read += 1;
    }
    Some(written)
}

/// True when NGINX's normalized `uri` differs from the raw request `path` only
/// by percent-decoding and/or merged slashes. Otherwise the module clears
/// `valid_unparsed_uri`, so the proxied URI is rebuilt from `r->uri`.
pub fn only_decoded_or_merged(path: &[u8], uri: &[u8], scratch: &mut [u8]) -> bool {
    let Some(length) = decode_into(path, scratch) else {
        return false;
    };
    if &scratch[..length] == uri {
        return true;
    }
    let mut written = 0;
    for read in 0..length {
        let byte = scratch[read];
        if byte == b'/' && written > 0 && scratch[written - 1] == b'/' {
            continue;
        }
        scratch[written] = byte;
        written += 1;
    }
    &scratch[..written] == uri
}

#[cfg(test)]
mod tests {
    use super::*;

    fn check(path: &str, uri: &str) -> bool {
        let mut scratch = vec![0u8; path.len() + 1];
        only_decoded_or_merged(path.as_bytes(), uri.as_bytes(), &mut scratch)
    }

    #[test]
    fn decodes_valid_escapes_only() {
        let mut out = [0u8; 16];
        let n = decode_into(b"/a%2Fb%zz%4", &mut out).unwrap();
        assert_eq!(&out[..n], b"/a/b%zz%4");
        assert!(decode_into(b"/abc", &mut [0u8; 2]).is_none());
    }

    #[test]
    fn accepts_decoded_or_merged_paths() {
        assert!(check("/auth/item", "/auth/item"));
        assert!(check("/auth/a%2Fb", "/auth/a/b"));
        assert!(check("/auth//item", "/auth/item"));
        assert!(check("/auth/%41", "/auth/A"));
    }

    #[test]
    fn rejects_dot_segment_rewrites() {
        // NGINX resolves dot segments in r->uri; the raw path must be re-escaped.
        assert!(!check("/auth/a/../item", "/auth/item"));
    }
}
