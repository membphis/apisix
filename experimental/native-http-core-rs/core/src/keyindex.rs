// SPDX-License-Identifier: Apache-2.0
//! Consumer key index ported from `module/key_auth.c` (`ac_key_auth_prepare`):
//! NGINX's `ngx_hash_key`, a power-of-two bucket mask and head insertion, so
//! the latest duplicate key wins exactly like the Lua consumer index.

/// `ngx_hash_key`: `key = key * 31 + byte` over `ngx_uint_t` (64-bit here).
#[inline]
pub fn ngx_hash_key(data: &[u8]) -> u64 {
    let mut key: u64 = 0;
    for &byte in data {
        key = key.wrapping_mul(31).wrapping_add(u64::from(byte));
    }
    key
}

const NONE: u32 = u32::MAX;

#[derive(Debug)]
pub struct KeyIndex {
    mask: u64,
    heads: Vec<u32>,
    next: Vec<u32>,
    keys: Vec<Vec<u8>>,
    values: Vec<usize>,
}

impl KeyIndex {
    /// Builds the index from `(key, value)` pairs in configuration order.
    pub fn build(entries: Vec<(Vec<u8>, usize)>) -> KeyIndex {
        let count = entries.len();
        let mut size: usize = 1;
        while size < count.saturating_mul(2) && size < 0x4000_0000 {
            size <<= 1;
        }
        let mut index = KeyIndex {
            mask: (size - 1) as u64,
            heads: vec![NONE; size],
            next: Vec::with_capacity(count),
            keys: Vec::with_capacity(count),
            values: Vec::with_capacity(count),
        };
        for (key, value) in entries {
            let slot = (ngx_hash_key(&key) & index.mask) as usize;
            let id = index.keys.len() as u32;
            index.next.push(index.heads[slot]);
            index.heads[slot] = id;
            index.keys.push(key);
            index.values.push(value);
        }
        index
    }

    #[inline]
    pub fn find(&self, key: &[u8]) -> Option<usize> {
        let mut id = *self.heads.get((ngx_hash_key(key) & self.mask) as usize)?;
        while id != NONE {
            let entry = id as usize;
            if self.keys[entry] == key {
                return Some(self.values[entry]);
            }
            id = self.next[entry];
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_nginx_hash() {
        // ngx_hash_key("ab") = 'a' * 31 + 'b'
        assert_eq!(ngx_hash_key(b"ab"), 97 * 31 + 98);
        assert_eq!(ngx_hash_key(b""), 0);
    }

    #[test]
    fn latest_duplicate_wins_and_empty_key_is_valid() {
        let index = KeyIndex::build(vec![
            (b"demo-key".to_vec(), 0),
            (b"other".to_vec(), 1),
            (b"demo-key".to_vec(), 2),
            (b"".to_vec(), 3),
        ]);
        assert_eq!(index.find(b"demo-key"), Some(2));
        assert_eq!(index.find(b"other"), Some(1));
        assert_eq!(index.find(b""), Some(3));
        assert_eq!(index.find(b"DEMO-KEY"), None);
    }

    #[test]
    fn empty_index() {
        let index = KeyIndex::build(Vec::new());
        assert_eq!(index.find(b"x"), None);
    }
}
