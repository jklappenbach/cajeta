// toolchain/debugger-attach debuggee (bench/toolchain/Debugger.md): a running
// process that calls tick() every millisecond, for gdb to attach to and break in.
// PR_SET_PTRACER_ANY (prctl, from libc without a crate) lets a non-parent
// debugger attach under Yama ptrace_scope=1.
use std::io::Write;

extern "C" {
    fn prctl(option: i32, arg2: u64, arg3: u64, arg4: u64, arg5: u64) -> i32;
}

#[no_mangle]
#[inline(never)]
pub extern "C" fn tick(n: i64) -> i64 {
    n * 3 + 1
}

fn main() {
    unsafe { prctl(0x59616d61, u64::MAX, 0, 0, 0); }
    println!("ready");
    std::io::stdout().flush().unwrap();
    let mut acc: i64 = 0;
    let mut i: i64 = 0;
    loop {
        acc = std::hint::black_box(acc.wrapping_add(tick(i)));
        i += 1;
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
}
