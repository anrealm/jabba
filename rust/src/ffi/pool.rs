//! A small process-wide thread pool that outlives every run: exported nodes run on it when
//! their host provides no `post`. Deliberately never shut down.

use std::collections::VecDeque;
use std::sync::{Condvar, Mutex, OnceLock};
use std::thread;

use crate::ctx::lock;

type Job = Box<dyn FnOnce() + Send>;

struct Pool {
    jobs: Mutex<VecDeque<Job>>,
    ready: Condvar,
}

fn pool() -> &'static Pool {
    static POOL: OnceLock<&'static Pool> = OnceLock::new();
    POOL.get_or_init(|| {
        let pool: &'static Pool = Box::leak(Box::new(Pool {
            jobs: Mutex::new(VecDeque::new()),
            ready: Condvar::new(),
        }));
        let threads = thread::available_parallelism()
            .map_or(2, |n| n.get())
            .clamp(2, 8);
        for i in 0..threads {
            thread::Builder::new()
                .name(format!("jabba-ffi-{i}"))
                .spawn(move || loop {
                    let job = {
                        let mut jobs = lock(&pool.jobs);
                        loop {
                            if let Some(job) = jobs.pop_front() {
                                break job;
                            }
                            jobs = pool.ready.wait(jobs).unwrap_or_else(|e| e.into_inner());
                        }
                    };
                    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(job));
                    // keep the thread
                })
                .expect("failed to start a jabba ffi thread");
        }
        pool
    })
}

pub(crate) fn post(job: Job) {
    let p = pool();
    lock(&p.jobs).push_back(job);
    p.ready.notify_one();
}
