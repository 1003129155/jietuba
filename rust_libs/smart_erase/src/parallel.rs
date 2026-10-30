//! Scoped worker threads. Work is split by fixed rules, never by thread count, so every machine
//! produces the same pixels.

use std::sync::atomic::{AtomicUsize, Ordering};

fn workers() -> usize {
    std::thread::available_parallelism().map_or(1, |n| n.get()).min(8)
}

/// Calls `f(start, chunk)` for consecutive chunks of `data`, chunks of at least `min_chunk` items.
pub(crate) fn for_each_chunk<T: Send, F: Fn(usize, &mut [T]) + Sync>(data: &mut [T], min_chunk: usize, f: F) {
    let threads = workers().min(data.len().div_ceil(min_chunk.max(1))).max(1);
    if threads == 1 {
        f(0, data);
        return;
    }
    let size = data.len().div_ceil(threads);
    std::thread::scope(|scope| {
        for (n, chunk) in data.chunks_mut(size).enumerate() {
            let f = &f;
            scope.spawn(move || f(n * size, chunk));
        }
    });
}

/// Calls `f(item)` for every item, any order, any thread.
pub(crate) fn for_each<T: Sync, F: Fn(&T) + Sync>(items: &[T], f: F) {
    let threads = workers().min(items.len());
    if threads <= 1 {
        items.iter().for_each(f);
        return;
    }
    let next = AtomicUsize::new(0);
    std::thread::scope(|scope| {
        for _ in 0..threads {
            scope.spawn(|| loop {
                let k = next.fetch_add(1, Ordering::Relaxed);
                let Some(item) = items.get(k) else { break };
                f(item);
            });
        }
    });
}
