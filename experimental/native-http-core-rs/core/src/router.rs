// SPDX-License-Identifier: Apache-2.0
//! Compressed URI prefix tree and candidate precedence ported from
//! `module/routes.c` (`ac_index_insert`, `ac_build_index`, `ac_match_native`,
//! `ac_candidate_order`). Host, method and remote-address predicates need the
//! NGINX request, so the module evaluates them through the `check` callback;
//! path matching and precedence are evaluated here.

use std::cmp::Ordering;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PathSpec {
    pub source: Vec<u8>,
    pub prefix_len: usize,
    pub exact: bool,
}

impl PathSpec {
    /// `ac_compile_path`. Patterns that need the regular-expression branch
    /// (a `*` before the end, or `:param` with the parameter router) are
    /// rejected: the pipeline contract only covers exact and prefix routes.
    pub fn compile(source: &[u8], parameter_router: bool) -> Result<PathSpec, String> {
        if source.first() != Some(&b'/') {
            return Err("route URI must start with '/'".into());
        }
        let mut spec = PathSpec { source: source.to_vec(), prefix_len: source.len(), exact: true };
        for (index, &byte) in source.iter().enumerate() {
            if byte == b'*' || (byte == b':' && parameter_router) {
                spec.prefix_len = index;
                spec.exact = false;
                if byte == b':' || index + 1 < source.len() {
                    return Err(format!(
                        "route pattern '{}' needs a regular expression, which the Rust pipeline does not compile",
                        String::from_utf8_lossy(source)
                    ));
                }
                break;
            }
        }
        Ok(spec)
    }

    #[inline]
    fn matches(&self, uri: &[u8]) -> bool {
        if self.exact {
            return self.source.as_slice() == uri;
        }
        uri.len() >= self.prefix_len && uri[..self.prefix_len] == self.source[..self.prefix_len]
    }
}

/// `ac_host_rank`: `None` when no host matches; exact hosts outrank
/// wildcards (`*.example.com`) and longer patterns outrank shorter ones.
/// `server` is NGINX's validated, lower-cased host; the full `Host` header is
/// only consulted for port-qualified patterns outside the host router.
pub fn host_rank(hosts: &[Vec<u8>], host_router: bool, server: &[u8], host_header: Option<&[u8]>) -> Option<i64> {
    host_match(hosts, host_router, server, host_header).map(|(rank, _)| rank)
}

/// `ac_host_rank` with the matched pattern: `(rank, index into hosts)`;
/// a route without hosts matches with rank 0 (index 0, unused).
pub fn host_match(hosts: &[Vec<u8>], host_router: bool, server: &[u8], host_header: Option<&[u8]>) -> Option<(i64, usize)> {
    if hosts.is_empty() {
        return Some((0, 0));
    }
    let mut best: Option<(i64, usize)> = None;
    for (index, candidate) in hosts.iter().enumerate() {
        let host = match host_header {
            Some(header) if !host_router && candidate.contains(&b':') => header,
            _ => server,
        };
        let rank = if let Some(suffix) = candidate.strip_prefix(b"*") {
            if host.len() < suffix.len() || !host[host.len() - suffix.len()..].eq_ignore_ascii_case(suffix) {
                continue;
            }
            candidate.len() as i64
        } else {
            if !host.eq_ignore_ascii_case(candidate) {
                continue;
            }
            candidate.len() as i64 + 65536
        };
        if best.is_none_or(|(current, _)| rank > current) {
            best = Some((rank, index));
        }
    }
    best
}

#[derive(Debug)]
struct Entry {
    route: usize,
    path: usize,
    sequence: usize,
}

#[derive(Debug)]
struct Node {
    edge: Vec<u8>,
    children: Vec<usize>,
    entries: Vec<Entry>,
}

#[derive(Debug, Clone, Copy)]
struct Candidate {
    host_rank: i64,
    exact: bool,
    prefix_len: usize,
    priority: i64,
    source_len: usize,
    sequence: usize,
    route: usize,
    path: usize,
}

/// `ac_candidate_order`: `Less` means `left` is preferred.
fn order(left: &Candidate, right: &Candidate) -> Ordering {
    right
        .host_rank
        .cmp(&left.host_rank)
        .then_with(|| right.exact.cmp(&left.exact))
        .then_with(|| right.prefix_len.cmp(&left.prefix_len))
        .then_with(|| right.priority.cmp(&left.priority))
        .then_with(|| right.source_len.cmp(&left.source_len))
        .then_with(|| left.sequence.cmp(&right.sequence))
}

#[derive(Debug)]
pub struct Router {
    nodes: Vec<Node>,
    paths: Vec<Vec<PathSpec>>,
    priorities: Vec<i64>,
}

impl Router {
    /// Builds the index from `(priority, paths)` per route, in route order.
    pub fn build(routes: Vec<(i64, Vec<PathSpec>)>) -> Router {
        let mut router = Router {
            nodes: vec![Node { edge: Vec::new(), children: Vec::new(), entries: Vec::new() }],
            paths: Vec::with_capacity(routes.len()),
            priorities: Vec::with_capacity(routes.len()),
        };
        let mut sequence = 0;
        for (route, (priority, paths)) in routes.into_iter().enumerate() {
            for (path, spec) in paths.iter().enumerate() {
                let node = router.insert(0, &spec.source[..spec.prefix_len]);
                router.nodes[node].entries.push(Entry { route, path, sequence });
                sequence += 1;
            }
            router.paths.push(paths);
            router.priorities.push(priority);
        }
        router
    }

    /// The configured URI pattern of `(route, path)` (the matched `_path`).
    #[inline]
    pub fn path_source(&self, route: usize, path: usize) -> &[u8] {
        &self.paths[route][path].source
    }

    fn insert(&mut self, parent: usize, key: &[u8]) -> usize {
        if key.is_empty() {
            return parent;
        }
        let found = self.nodes[parent]
            .children
            .iter()
            .copied()
            .find(|&child| self.nodes[child].edge.first() == key.first());
        if let Some(child) = found {
            let edge_len = self.nodes[child].edge.len();
            let mut common = 1;
            while common < edge_len && common < key.len() && self.nodes[child].edge[common] == key[common] {
                common += 1;
            }
            if common < edge_len {
                let split = Node {
                    edge: self.nodes[child].edge[common..].to_vec(),
                    children: std::mem::take(&mut self.nodes[child].children),
                    entries: std::mem::take(&mut self.nodes[child].entries),
                };
                let split_index = self.nodes.len();
                self.nodes.push(split);
                self.nodes[child].edge.truncate(common);
                self.nodes[child].children = vec![split_index];
            }
            return self.insert(child, &key[common..]);
        }
        let index = self.nodes.len();
        self.nodes.push(Node { edge: key.to_vec(), children: Vec::new(), entries: Vec::new() });
        self.nodes[parent].children.push(index);
        index
    }

    /// Walks the tree along `uri`. `check(route)` evaluates the host, method
    /// and remote-address predicates and returns the host rank, or `None`
    /// when the route is not eligible. Returns `(route, path)` of the best
    /// candidate.
    #[inline]
    pub fn find<F>(&self, uri: &[u8], mut check: F) -> Option<(usize, usize)>
    where
        F: FnMut(usize) -> Option<i64>,
    {
        let mut node = 0;
        let mut offset = 0;
        let mut best: Option<Candidate> = None;
        loop {
            for entry in &self.nodes[node].entries {
                let Some(host_rank) = check(entry.route) else { continue };
                let path = &self.paths[entry.route][entry.path];
                if !path.matches(uri) {
                    continue;
                }
                let candidate = Candidate {
                    host_rank,
                    exact: path.exact,
                    prefix_len: path.prefix_len,
                    priority: self.priorities[entry.route],
                    source_len: path.source.len(),
                    sequence: entry.sequence,
                    route: entry.route,
                    path: entry.path,
                };
                if best.as_ref().is_none_or(|current| order(&candidate, current) == Ordering::Less) {
                    best = Some(candidate);
                }
            }
            if offset == uri.len() {
                break;
            }
            let rest = &uri[offset..];
            let next = self.nodes[node].children.iter().copied().find(|&child| {
                let edge = &self.nodes[child].edge;
                edge.len() <= rest.len() && rest[..edge.len()] == edge[..]
            });
            match next {
                Some(child) => {
                    offset += self.nodes[child].edge.len();
                    node = child;
                }
                None => break,
            }
        }
        best.map(|candidate| (candidate.route, candidate.path))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn paths(items: &[&str]) -> Vec<PathSpec> {
        items.iter().map(|p| PathSpec::compile(p.as_bytes(), false).unwrap()).collect()
    }

    #[test]
    fn compiles_exact_prefix_and_rejects_regex_patterns() {
        let exact = PathSpec::compile(b"/a/b", false).unwrap();
        assert!(exact.exact && exact.prefix_len == 4);
        let prefix = PathSpec::compile(b"/auth/*", false).unwrap();
        assert!(!prefix.exact && prefix.prefix_len == 6);
        assert!(PathSpec::compile(b"/a/*/b", false).is_err());
        assert!(PathSpec::compile(b"/a/:id", true).is_err());
        assert!(PathSpec::compile(b"/a/:id", false).unwrap().exact);
        assert!(PathSpec::compile(b"auth", false).is_err());
    }

    #[test]
    fn prefix_routes_do_not_overmatch_and_longest_prefix_wins() {
        let router = Router::build(vec![
            (0, paths(&["/auth/*"])),
            (0, paths(&["/authentic/*"])),
            (0, paths(&["/auth/deep/*"])),
            (0, paths(&["/"])),
        ]);
        let any = |_| Some(0);
        assert_eq!(router.find(b"/auth/item", any), Some((0, 0)));
        assert_eq!(router.find(b"/authentic/item", any), Some((1, 0)));
        assert_eq!(router.find(b"/auth/deep/x", any), Some((2, 0)));
        assert_eq!(router.find(b"/", any), Some((3, 0)));
        assert_eq!(router.find(b"/outside", any), None);
        assert_eq!(router.find(b"/auth", any), None);
    }

    #[test]
    fn exact_before_prefix_then_host_rank_priority_and_sequence() {
        let router = Router::build(vec![
            (0, paths(&["/a/*"])),
            (0, paths(&["/a/b"])),
            (5, paths(&["/x/*"])),
            (9, paths(&["/x/*"])),
            (0, paths(&["/h/*"])),
            (0, paths(&["/h/*"])),
        ]);
        let any = |_| Some(0);
        assert_eq!(router.find(b"/a/b", any), Some((1, 0)));
        assert_eq!(router.find(b"/x/1", any), Some((3, 0)));
        // A host-qualified route outranks a host-less one with the same path.
        assert_eq!(router.find(b"/h/1", |route| Some(if route == 5 { 65546 } else { 0 })), Some((5, 0)));
        // Ineligible routes are skipped.
        assert_eq!(router.find(b"/h/1", |route| if route == 4 { None } else { Some(0) }), Some((5, 0)));
        // Equal candidates keep configuration order.
        assert_eq!(router.find(b"/h/1", any), Some((4, 0)));
    }

    #[test]
    fn host_match_reports_the_best_pattern() {
        let hosts = vec![b"*.example.com".to_vec(), b"api.example.com".to_vec(), b"other.test".to_vec()];
        // Exact beats wildcard; the index names the matched `_host` pattern.
        assert_eq!(host_match(&hosts, true, b"api.example.com", None), Some((15 + 65536, 1)));
        assert_eq!(host_match(&hosts, true, b"www.example.com", None), Some((13, 0)));
        assert_eq!(host_match(&hosts, true, b"nowhere", None), None);
        assert_eq!(host_match(&[], true, b"any", None), Some((0, 0)));
        assert_eq!(host_rank(&hosts, true, b"API.EXAMPLE.COM", None), Some(15 + 65536));
        let router = Router::build(vec![(0, paths(&["/a/*", "/b"]))]);
        assert_eq!(router.path_source(0, 1), b"/b");
    }

    #[test]
    fn split_edges_keep_entries() {
        let router = Router::build(vec![
            (0, paths(&["/abcd"])),
            (0, paths(&["/ab"])),
            (0, paths(&["/abce/*"])),
        ]);
        let any = |_| Some(0);
        assert_eq!(router.find(b"/abcd", any), Some((0, 0)));
        assert_eq!(router.find(b"/ab", any), Some((1, 0)));
        assert_eq!(router.find(b"/abce/z", any), Some((2, 0)));
        assert_eq!(router.find(b"/abc", any), None);
    }
}
