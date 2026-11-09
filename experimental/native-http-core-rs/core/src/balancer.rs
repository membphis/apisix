// SPDX-License-Identifier: Apache-2.0
//! Upstream peer selection ported from `module/upstream.c`: `ac_node_eligible`,
//! `ac_peer_get` (smooth weighted round robin, least_conn, ewma with priority
//! tiers), `ac_peer_free` and the EWMA feedback of `ac_upstream_log`.
//!
//! Node state lives in the configuration generation, like the C `ac_node_t`;
//! NGINX workers are single-threaded, so `Cell` is sufficient.

use std::cell::Cell;
use std::net::IpAddr;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    RoundRobin,
    LeastConn,
    Ewma,
}

#[derive(Debug, Default)]
pub struct NodeState {
    pub current: Cell<i64>,
    pub active: Cell<u64>,
    pub fails: Cell<u64>,
    pub checked: Cell<u64>,
    pub ewma: Cell<f64>,
}

#[derive(Debug)]
pub struct Node {
    /// Host as configured (an IP literal; IPv6 keeps any brackets).
    pub host: Vec<u8>,
    pub port: u16,
    pub weight: u32,
    pub priority: i64,
    pub ip: IpAddr,
    /// `host:port` or `[host]:port`, used as the peer name and EWMA key.
    pub name: Vec<u8>,
    pub state: NodeState,
}

#[derive(Debug)]
pub struct Upstream {
    pub kind: Kind,
    pub nodes: Vec<Node>,
    /// `pass_host = rewrite` value; `None` means pass the client Host.
    pub rewrite_host: Option<Vec<u8>>,
    pub retries: Option<u32>,
    pub retry_timeout_ms: u64,
    pub connect_timeout_ms: u64,
    pub send_timeout_ms: u64,
    pub read_timeout_ms: u64,
    pub max_fails: u64,
    pub fail_timeout_ms: u64,
}

/// Per-request retry bitmap. Up to 64 nodes need no heap allocation, which
/// keeps parity with the C implementation's request-pool bitmap.
#[derive(Debug)]
enum Tried {
    Inline(u64),
    Heap(Vec<u64>),
}

#[derive(Debug)]
pub struct Peer {
    tried: Tried,
    pub selected: Option<usize>,
    pub started_ms: u64,
}

impl Peer {
    pub fn new(nodes: usize, now_ms: u64) -> Peer {
        let tried = if nodes <= 64 { Tried::Inline(0) } else { Tried::Heap(vec![0; nodes.div_ceil(64)]) };
        Peer { tried, selected: None, started_ms: now_ms }
    }

    #[inline]
    fn is_tried(&self, index: usize) -> bool {
        let bit = 1u64 << (index % 64);
        match &self.tried {
            Tried::Inline(word) => word & bit != 0,
            Tried::Heap(words) => words.get(index / 64).is_some_and(|word| word & bit != 0),
        }
    }

    #[inline]
    fn mark(&mut self, index: usize) {
        let bit = 1u64 << (index % 64);
        match &mut self.tried {
            Tried::Inline(word) => *word |= bit,
            Tried::Heap(words) => {
                if let Some(word) = words.get_mut(index / 64) {
                    *word |= bit;
                }
            }
        }
    }
}

impl Upstream {
    #[inline]
    pub fn eligible(&self, peer: &Peer, index: usize, now_ms: u64) -> bool {
        let node = &self.nodes[index];
        if peer.is_tried(index) || node.weight == 0 {
            return false;
        }
        !(self.max_fails > 0
            && self.nodes.len() > 1
            && node.state.fails.get() >= self.max_fails
            && now_ms.wrapping_sub(node.state.checked.get()) <= self.fail_timeout_ms)
    }

    /// Selects the next peer, marks it tried and counts it active.
    pub fn pick(&self, peer: &mut Peer, now_ms: u64) -> Option<usize> {
        let mut best_priority: Option<i64> = None;
        for index in 0..self.nodes.len() {
            if self.eligible(peer, index, now_ms) {
                let priority = self.nodes[index].priority;
                if best_priority.is_none_or(|best| priority > best) {
                    best_priority = Some(priority);
                }
            }
        }
        let best_priority = best_priority?;
        let mut total: i64 = 0;
        let mut selected: Option<usize> = None;
        let mut best_score = f64::INFINITY;
        for index in 0..self.nodes.len() {
            let node = &self.nodes[index];
            if node.priority != best_priority || !self.eligible(peer, index, now_ms) {
                continue;
            }
            let state = &node.state;
            let weight = f64::from(node.weight);
            let score = match self.kind {
                Kind::LeastConn => (1.0 + state.active.get() as f64) / weight,
                Kind::Ewma => {
                    let ewma = state.ewma.get();
                    (state.active.get() as f64 + 1.0) * if ewma > 0.0 { ewma } else { 1.0 } / weight
                }
                Kind::RoundRobin => {
                    state.current.set(state.current.get() + i64::from(node.weight));
                    total += i64::from(node.weight);
                    -(state.current.get() as f64)
                }
            };
            if selected.is_none() || score < best_score {
                selected = Some(index);
                best_score = score;
            }
        }
        let index = selected?;
        let state = &self.nodes[index].state;
        if self.kind == Kind::RoundRobin {
            state.current.set(state.current.get() - total);
        }
        peer.mark(index);
        state.active.set(state.active.get() + 1);
        peer.selected = Some(index);
        Some(index)
    }

    /// `ac_peer_free`: releases the selected peer, recording a failure.
    pub fn release(&self, peer: &mut Peer, failed: bool, now_ms: u64) {
        if let Some(index) = peer.selected.take() {
            let state = &self.nodes[index].state;
            if state.active.get() > 0 {
                state.active.set(state.active.get() - 1);
            }
            if failed {
                state.fails.set(state.fails.get() + 1);
                state.checked.set(now_ms);
            } else {
                state.fails.set(0);
            }
        }
    }

    /// EWMA feedback from the final upstream state (`ac_upstream_log`).
    pub fn record_response_time(&self, peer_name: &[u8], response_time_ms: f64) {
        if let Some(node) = self.nodes.iter().find(|node| node.name == peer_name) {
            let ewma = node.state.ewma.get();
            node.state.ewma.set(if ewma == 0.0 { response_time_ms } else { ewma * 0.9 + response_time_ms * 0.1 });
        }
    }

    /// Unset retries mean "every other node", as in the C and Lua balancers.
    pub fn effective_retries(&self) -> u32 {
        self.retries.unwrap_or_else(|| u32::try_from(self.nodes.len().saturating_sub(1)).unwrap_or(u32::MAX))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn node(port: u16, weight: u32, priority: i64) -> Node {
        Node {
            host: b"127.0.0.1".to_vec(),
            port,
            weight,
            priority,
            ip: "127.0.0.1".parse().unwrap(),
            name: format!("127.0.0.1:{port}").into_bytes(),
            state: NodeState::default(),
        }
    }

    fn upstream(kind: Kind, nodes: Vec<Node>) -> Upstream {
        Upstream {
            kind,
            nodes,
            rewrite_host: None,
            retries: None,
            retry_timeout_ms: 0,
            connect_timeout_ms: 6000,
            send_timeout_ms: 6000,
            read_timeout_ms: 6000,
            max_fails: 0,
            fail_timeout_ms: 10000,
        }
    }

    #[test]
    fn smooth_round_robin_distributes_by_weight() {
        let up = upstream(Kind::RoundRobin, vec![node(1, 1, 0), node(2, 1, 0), node(3, 1, 0)]);
        let mut counts = [0; 3];
        for _ in 0..300 {
            let mut peer = Peer::new(3, 0);
            let index = up.pick(&mut peer, 0).unwrap();
            counts[index] += 1;
            up.release(&mut peer, false, 0);
        }
        assert_eq!(counts, [100, 100, 100]);
        let weighted = upstream(Kind::RoundRobin, vec![node(1, 5, 0), node(2, 1, 0), node(3, 1, 0)]);
        let sequence: Vec<usize> = (0..7)
            .map(|_| {
                let mut peer = Peer::new(3, 0);
                let index = weighted.pick(&mut peer, 0).unwrap();
                weighted.release(&mut peer, false, 0);
                index
            })
            .collect();
        assert_eq!(sequence, vec![0, 0, 1, 0, 2, 0, 0]);
    }

    #[test]
    fn higher_priority_first_then_retry_falls_back() {
        let up = upstream(Kind::RoundRobin, vec![node(1999, 1, 1), node(1980, 1, 0)]);
        let mut peer = Peer::new(2, 0);
        assert_eq!(up.pick(&mut peer, 0), Some(0));
        up.release(&mut peer, true, 0);
        assert_eq!(up.pick(&mut peer, 0), Some(1));
        up.release(&mut peer, false, 0);
        assert_eq!(up.pick(&mut peer, 0), None);
        assert_eq!(up.effective_retries(), 1);
    }

    #[test]
    fn zero_weight_is_ineligible_and_least_conn_prefers_idle() {
        let up = upstream(Kind::LeastConn, vec![node(1, 0, 0), node(2, 1, 0), node(3, 1, 0)]);
        let mut first = Peer::new(3, 0);
        assert_eq!(up.pick(&mut first, 0), Some(1));
        let mut second = Peer::new(3, 0);
        assert_eq!(up.pick(&mut second, 0), Some(2));
    }

    #[test]
    fn large_upstreams_use_heap_bitmap() {
        let nodes = (0..70).map(|i| node(1000 + i, 1, 0)).collect();
        let up = upstream(Kind::RoundRobin, nodes);
        let mut peer = Peer::new(70, 0);
        let mut seen = std::collections::HashSet::new();
        while let Some(index) = up.pick(&mut peer, 0) {
            assert!(seen.insert(index));
            up.release(&mut peer, true, 0);
        }
        assert_eq!(seen.len(), 70);
    }
}
