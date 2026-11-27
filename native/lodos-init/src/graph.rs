use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fmt;

use crate::unit::UnitFile;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GraphError {
    MissingRequired { unit: String, missing: String },
    Cycle(Vec<String>),
}

impl fmt::Display for GraphError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            GraphError::MissingRequired { unit, missing } => {
                write!(f, "unit {} requires missing unit {}", unit, missing)
            }
            GraphError::Cycle(nodes) => {
                write!(f, "dependency cycle detected: {}", nodes.join(" -> "))
            }
        }
    }
}

impl std::error::Error for GraphError {}

/// Expand `requested` units transitively via `Requires` and `Wants`, then return
/// a deterministic topological startup order. Returns `GraphError::Cycle` if a
/// dependency/ordering cycle exists.
pub fn resolve_start_order(
    requested: &[String],
    units: &BTreeMap<String, UnitFile>,
) -> Result<Vec<String>, GraphError> {
    let mut active_set: BTreeSet<String> = BTreeSet::new();
    let mut queue: VecDeque<String> = VecDeque::new();

    for name in requested {
        if units.contains_key(name) && active_set.insert(name.clone()) {
            queue.push_back(name.clone());
        }
    }

    while let Some(name) = queue.pop_front() {
        let Some(unit) = units.get(&name) else {
            continue;
        };
        for req in &unit.unit.requires {
            if !units.contains_key(req) {
                return Err(GraphError::MissingRequired {
                    unit: name.clone(),
                    missing: req.clone(),
                });
            }
            if active_set.insert(req.clone()) {
                queue.push_back(req.clone());
            }
        }
        for want in &unit.unit.wants {
            if units.contains_key(want) && active_set.insert(want.clone()) {
                queue.push_back(want.clone());
            }
        }
    }

    topo_sort_subset(&active_set, units)
}

/// Topologically sort `nodes` based on `After` (hard ordering) and `Requires`
/// (ordering unless reversed by an explicit `After`).
pub fn topo_sort_subset(
    nodes: &BTreeSet<String>,
    units: &BTreeMap<String, UnitFile>,
) -> Result<Vec<String>, GraphError> {
    let mut edges: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    let mut in_degree: BTreeMap<String, usize> = BTreeMap::new();

    for n in nodes {
        edges.insert(n.clone(), BTreeSet::new());
        in_degree.insert(n.clone(), 0);
    }

    // Collect explicit After edges first: if A has After=B, edge is B -> A.
    let mut explicit_after: BTreeSet<(String, String)> = BTreeSet::new();
    for a in nodes {
        let Some(u) = units.get(a) else { continue };
        for b in &u.unit.after {
            if nodes.contains(b) {
                explicit_after.insert((b.clone(), a.clone()));
            }
        }
    }

    for (b, a) in &explicit_after {
        if b == a {
            return Err(GraphError::Cycle(vec![a.clone(), a.clone()]));
        }
        if edges.get_mut(b).unwrap().insert(a.clone()) {
            *in_degree.get_mut(a).unwrap() += 1;
        }
    }

    // Add Requires edges B -> A unless an explicit After says A -> B.
    for a in nodes {
        let Some(u) = units.get(a) else { continue };
        for b in &u.unit.requires {
            if !nodes.contains(b) {
                continue;
            }
            if b == a {
                return Err(GraphError::Cycle(vec![a.clone(), a.clone()]));
            }
            if explicit_after.contains(&(a.clone(), b.clone())) {
                continue;
            }
            if edges.get_mut(b).unwrap().insert(a.clone()) {
                *in_degree.get_mut(a).unwrap() += 1;
            }
        }
    }

    // Kahn's algorithm with deterministic alphabetical tie-breaking.
    let mut ready: BTreeSet<String> = in_degree
        .iter()
        .filter_map(|(k, &deg)| if deg == 0 { Some(k.clone()) } else { None })
        .collect();

    let mut order = Vec::with_capacity(nodes.len());
    while let Some(curr) = ready.pop_first() {
        order.push(curr.clone());
        if let Some(neighbors) = edges.get(&curr) {
            for next in neighbors {
                let deg = in_degree.get_mut(next).unwrap();
                *deg -= 1;
                if *deg == 0 {
                    ready.insert(next.clone());
                }
            }
        }
    }

    if order.len() != nodes.len() {
        let remaining: BTreeSet<String> = nodes
            .iter()
            .filter(|n| !order.contains(*n))
            .cloned()
            .collect();
        let cycle = find_cycle(&remaining, &edges);
        return Err(GraphError::Cycle(cycle));
    }

    Ok(order)
}

fn find_cycle(
    remaining: &BTreeSet<String>,
    edges: &BTreeMap<String, BTreeSet<String>>,
) -> Vec<String> {
    let mut visited = BTreeSet::new();
    let mut stack = Vec::new();
    let mut on_stack = BTreeSet::new();

    for start in remaining {
        if visited.contains(start) {
            continue;
        }
        if let Some(cycle) = dfs_cycle(start, remaining, edges, &mut visited, &mut on_stack, &mut stack) {
            return cycle;
        }
    }
    remaining.iter().cloned().collect()
}

fn dfs_cycle(
    node: &str,
    remaining: &BTreeSet<String>,
    edges: &BTreeMap<String, BTreeSet<String>>,
    visited: &mut BTreeSet<String>,
    on_stack: &mut BTreeSet<String>,
    stack: &mut Vec<String>,
) -> Option<Vec<String>> {
    visited.insert(node.to_string());
    on_stack.insert(node.to_string());
    stack.push(node.to_string());

    if let Some(neighbors) = edges.get(node) {
        for next in neighbors {
            if !remaining.contains(next) {
                continue;
            }
            if !visited.contains(next) {
                if let Some(cycle) = dfs_cycle(next, remaining, edges, visited, on_stack, stack) {
                    return Some(cycle);
                }
            } else if on_stack.contains(next) {
                if let Some(pos) = stack.iter().position(|x| x == next) {
                    let mut c = stack[pos..].to_vec();
                    c.push(next.clone());
                    return Some(c);
                }
            }
        }
    }

    stack.pop();
    on_stack.remove(node);
    None
}

/// Compute reverse dependency order for stopping active units.
/// If `A` has `After=B` (or `Requires=B`), `A` is stopped before `B`.
/// Falls back to reverse of `started_history` if topological sort cannot be built.
pub fn resolve_stop_order(
    running: &[String],
    units: &BTreeMap<String, UnitFile>,
    started_history: &[String],
) -> Vec<String> {
    let set: BTreeSet<String> = running.iter().cloned().collect();
    if let Ok(mut start_order) = topo_sort_subset(&set, units) {
        start_order.reverse();
        return start_order;
    }
    let mut out = Vec::new();
    for u in started_history.iter().rev() {
        if set.contains(u) && !out.contains(u) {
            out.push(u.clone());
        }
    }
    for u in running.iter().rev() {
        if !out.contains(u) {
            out.push(u.clone());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::unit::parse_unit_str;
    use std::path::Path;

    fn mk(name: &str, body: &str) -> (String, UnitFile) {
        let u = parse_unit_str(name, Path::new(name), body).unwrap();
        (u.name.clone(), u)
    }

    #[test]
    fn topo_sort_respects_after_and_requires() {
        let mut units = BTreeMap::new();
        units.insert(
            "a.service".into(),
            mk("a.service", "[Unit]\nAfter=b.service\nRequires=b.service\n[Service]\nExecStart=/bin/true\n").1,
        );
        units.insert(
            "b.service".into(),
            mk("b.service", "[Unit]\nAfter=c.service\nWants=c.service\n[Service]\nExecStart=/bin/true\n").1,
        );
        units.insert(
            "c.service".into(),
            mk("c.service", "[Service]\nExecStart=/bin/true\n").1,
        );
        let order = resolve_start_order(&["a.service".into()], &units).unwrap();
        assert_eq!(order, vec!["c.service", "b.service", "a.service"]);
        let stop = resolve_stop_order(&order, &units, &order);
        assert_eq!(stop, vec!["a.service", "b.service", "c.service"]);
    }

    #[test]
    fn detects_dependency_cycle() {
        let mut units = BTreeMap::new();
        units.insert(
            "a.service".into(),
            mk("a.service", "[Unit]\nAfter=b.service\n[Service]\nExecStart=/bin/true\n").1,
        );
        units.insert(
            "b.service".into(),
            mk("b.service", "[Unit]\nAfter=a.service\n[Service]\nExecStart=/bin/true\n").1,
        );
        let err = resolve_start_order(&["a.service".into(), "b.service".into()], &units)
            .expect_err("must detect cycle");
        match err {
            GraphError::Cycle(nodes) => {
                assert!(nodes.len() >= 3);
                assert_eq!(nodes.first(), nodes.last());
            }
            other => panic!("expected cycle, got {:?}", other),
        }
    }

    #[test]
    fn missing_requires_errors_but_missing_wants_ok() {
        let mut units = BTreeMap::new();
        units.insert(
            "a.service".into(),
            mk("a.service", "[Unit]\nRequires=missing.service\n[Service]\nExecStart=/bin/true\n").1,
        );
        units.insert(
            "b.service".into(),
            mk("b.service", "[Unit]\nWants=missing.service\n[Service]\nExecStart=/bin/true\n").1,
        );
        assert!(matches!(
            resolve_start_order(&["a.service".into()], &units),
            Err(GraphError::MissingRequired { .. })
        ));
        assert_eq!(
            resolve_start_order(&["b.service".into()], &units).unwrap(),
            vec!["b.service"]
        );
    }
}
