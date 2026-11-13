//! Assemble the AP startup trampoline before the kernel is compiled.
//!
//! `smp.rs` pulls the machine code in with `include_bytes!("ap_trampoline.bin")`,
//! so the file must exist when rustc runs. Generating it here (rather than
//! committing the binary) keeps the `.asm` source as the single source of
//! truth and makes a fresh clone buildable without manual steps.

use std::path::Path;
use std::process::Command;

fn assemble(src: &str, dst: &str) {
    let asm = Path::new(src);
    println!("cargo:rerun-if-changed={src}");

    if !asm.exists() {
        panic!("missing assembly source {src}");
    }

    // nasm is the assembler used by the original hand-verified offsets; fall
    // back to `as` only if it is absent, since the sources use NASM syntax.
    let status = Command::new("nasm")
        .args(["-f", "bin", "-o", dst, src])
        .status();

    match status {
        Ok(s) if s.success() => {}
        Ok(s) => panic!("nasm failed for {src} with status {s}"),
        Err(e) => panic!(
            "failed to run nasm ({e}). Install it (apt-get install nasm / pacman -S nasm)."
        ),
    }

    println!("cargo:rerun-if-changed={dst}");
}

fn main() {
    let manifest = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let out = Path::new(&manifest).join("src");
    assemble(
        out.join("ap_trampoline.asm").to_str().unwrap(),
        out.join("ap_trampoline.bin").to_str().unwrap(),
    );
}
