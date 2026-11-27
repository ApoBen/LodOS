use std::env;
use std::path::Path;
use std::process;

use lodos_rootfs::extract_tarball_with_progress;

fn usage() -> ! {
    eprintln!("usage: lodos-rootfs extract [--progress] <tarball> <dest>");
    process::exit(2);
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() || args[0] != "extract" {
        usage();
    }

    let mut show_progress = false;
    let mut positional: Vec<&str> = Vec::new();
    for arg in &args[1..] {
        if arg == "--progress" {
            show_progress = true;
        } else if arg == "-h" || arg == "--help" {
            usage();
        } else {
            positional.push(arg.as_str());
        }
    }

    if positional.len() != 2 {
        usage();
    }

    let src = Path::new(positional[0]);
    let dst = Path::new(positional[1]);
    let mut last_pct: i32 = -1;

    let res = extract_tarball_with_progress(src, dst, |done, total| {
        if !show_progress || total == 0 {
            return;
        }
        let pct = ((done.saturating_mul(100)) / total).min(100) as i32;
        if pct >= last_pct + 5 || pct == 100 && last_pct != 100 {
            last_pct = pct;
            eprintln!("progress: {pct}% ({done}/{total} bytes)");
        }
    });

    match res {
        Ok(summary) => {
            println!("{summary}");
        }
        Err(e) => {
            eprintln!("lodos-rootfs error: {e}");
            process::exit(1);
        }
    }
}
