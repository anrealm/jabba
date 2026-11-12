//! One background thread that wakes runs at their deadlines.

use std::collections::BTreeMap;
use std::sync::{Condvar, Mutex, OnceLock};
use std::task::Waker;
use std::thread;
use std::time::Instant;

use crate::ctx::lock;

/// A registration in the timer; cancel it when it is no longer needed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct TimerKey {
    at: Instant,
    id: u64,
}

struct Timer {
    state: Mutex<State>,
    changed: Condvar,
}

#[derive(Default)]
struct State {
    next_id: u64,
    entries: BTreeMap<(Instant, u64), Waker>,
}

fn timer() -> &'static Timer {
    static TIMER: OnceLock<&'static Timer> = OnceLock::new();
    TIMER.get_or_init(|| {
        let timer: &'static Timer = Box::leak(Box::new(Timer {
            state: Mutex::default(),
            changed: Condvar::new(),
        }));
        thread::Builder::new()
            .name("jabba-timer".into())
            .spawn(move || timer.run())
            .expect("failed to start the jabba timer thread");
        timer
    })
}

/// Wake `waker` at `at` (or soon after).
pub(crate) fn register(at: Instant, waker: Waker) -> TimerKey {
    let t = timer();
    let mut state = lock(&t.state);
    let id = state.next_id;
    state.next_id += 1;
    let earliest = state
        .entries
        .keys()
        .next()
        .map_or(true, |&(first, _)| at < first);
    state.entries.insert((at, id), waker);
    drop(state);
    if earliest {
        t.changed.notify_one();
    }
    TimerKey { at, id }
}

pub(crate) fn cancel(key: TimerKey) {
    lock(&timer().state).entries.remove(&(key.at, key.id));
}

impl Timer {
    fn run(&self) {
        let mut state = lock(&self.state);
        loop {
            let now = Instant::now();
            let mut due = Vec::new();
            while let Some(entry) = state.entries.first_entry() {
                if entry.key().0 > now {
                    break;
                }
                due.push(entry.remove());
            }
            if !due.is_empty() {
                drop(state);
                for waker in due {
                    waker.wake();
                }
                state = lock(&self.state);
                continue;
            }
            state = match state.entries.keys().next() {
                Some(&(at, _)) => {
                    let wait = at.saturating_duration_since(now);
                    self.changed
                        .wait_timeout(state, wait)
                        .unwrap_or_else(|e| e.into_inner())
                        .0
                }
                None => self.changed.wait(state).unwrap_or_else(|e| e.into_inner()),
            };
        }
    }
}
