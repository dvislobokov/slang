//! Every ```` ```zell ```` block of the agent skills
//! (`agents/claude-code/skills/*/SKILL.md`) type-checks with `zell check`:
//! agents copy these snippets, so they must be valid Zell.

use std::path::{Path, PathBuf};

fn zell() -> Option<PathBuf> {
    let exe = PathBuf::from(env!("CARGO_BIN_EXE_zell-mcp")).with_file_name(format!("zell{}", std::env::consts::EXE_SUFFIX));
    exe.is_file().then_some(exe)
}

#[test]
fn skill_code_blocks_type_check() {
    let Some(zell) = zell() else {
        eprintln!("skipping: build the zell binary first (cargo build -p zell)");
        return;
    };
    let skills = Path::new(env!("CARGO_MANIFEST_DIR")).join("../claude-code/skills");
    let dir = std::env::temp_dir().join(format!("zell-skill-blocks-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let mut checked = 0;
    let mut failures = Vec::new();
    for skill in std::fs::read_dir(&skills).unwrap().flatten() {
        let Ok(text) = std::fs::read_to_string(skill.path().join("SKILL.md")) else { continue };
        for (i, block) in text.split("```zell\n").skip(1).enumerate() {
            let code = block.split("```").next().unwrap();
            let file = dir.join(format!("{}_{i}.zl", skill.file_name().to_string_lossy().replace('-', "_")));
            std::fs::write(&file, code).unwrap();
            let out = std::process::Command::new(&zell).arg("check").arg(&file).output().unwrap();
            if !out.status.success() {
                failures.push(format!("{} block {i}:\n{}", skill.path().display(), String::from_utf8_lossy(&out.stderr)));
            }
            checked += 1;
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
    assert!(checked >= 4, "found only {checked} blocks");
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}
