// Runs git-bash (MSYS2 runtime) inside the mxc ProcessContainer backend via
// the public v1::run API. This is the end-to-end reproduction for
// microsoft/mxc#1061 and buhuikongpan/dsh-win-gitbash#10.
//
// Usage: mxc_gitbash_run [command]
//   default command: "bash --version"

use mxc_sdk::v1::configs::ProcessContainerConfig;
use mxc_sdk::v1::{run, ContainerRequest, Containment, FilesystemPolicy, RunOptions, UiPolicy};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let command = if args.len() > 1 {
        args[1..].join(" ")
    } else {
        "bash --version".to_string()
    };

    // Locate git-bash's bin dir so `bash` resolves.
    let git_bash_bin = std::env::var("GIT_BASH_BIN")
        .unwrap_or_else(|_| "C:\\tools\\git-bash\\bin".to_string());

    // The container can only see git_bash_bin, its tree root and C:\Windows.
    // Give the workload a PATH inside those, or tools like tr/ls resolve into
    // C:\Program Files\Git\... which the policy hides -> "command not found".
    let workload_path = {
        use std::path::Path;
        let usr_bin = Path::new(&git_bash_bin)
            .parent()
            .map(|p| p.join("usr").join("bin"))
            .unwrap_or_default();
        let windir = std::env::var("WINDIR").unwrap_or_else(|_| "C:\\Windows".to_string());
        format!(
            "{};{};{}\\System32;{}",
            git_bash_bin,
            usr_bin.display(),
            windir,
            windir
        )
    };

    let request = ContainerRequest {
        filesystem: Some(FilesystemPolicy {
            readwrite_paths: vec![std::env::temp_dir().display().to_string()],
            readonly_paths: vec![
                git_bash_bin.clone(),
                git_bash_bin.replace("\\bin", ""),
                "C:\\Windows".to_string(),
            ],
            denied_paths: vec![],
            clear_policy_on_exit: None,
        }),
        containment: Containment::ProcessContainer({
            // Audit mode denial capture, enabled with PIG_DENIALS=1.
            let mut pc = ProcessContainerConfig::default();
            if std::env::var("PIG_DENIALS").is_ok() {
                let mut cd = mxc_sdk::v1::configs::CaptureDenials::default();
                cd.mode = mxc_sdk::v1::configs::CaptureDenialsMode::Allow;
                cd.output_path = Some(
                    std::env::temp_dir()
                        .join("mxc-gitbash-denials.json")
                        .to_string_lossy()
                        .into_owned(),
                );
                pc.capture_denials = Some(cd);
            }
            pc
        }),
        // mxc#1061: UI/Win32k must stay enabled.  With ui.disable=true
        // (the default), USER32's DllMain fails in the container and every
        // USER32-importing exe (git-bash, powershell) dies 0xC0000142
        // before main().
        ui: Some(UiPolicy {
            disable: false,
            ..Default::default()
        }),
        // Debug sessions need long runs: MXC_RUN_TIMEOUT_MS overrides the default.
        timeout_ms: Some(
            std::env::var("MXC_RUN_TIMEOUT_MS")
                .ok()
                .and_then(|s| s.parse::<u32>().ok())
                .unwrap_or(60000),
        ),
        environment: Some(vec![("PATH".to_string(), workload_path)]),
        inherit_default_environment: Some(true),
        ..ContainerRequest::new(command)
    };

    eprintln!("== mxc ProcessContainer run ==");
    eprintln!("command: {}", request.command);

    match run(request, RunOptions::default()) {
        Ok(result) => {
            eprintln!("outcome: {:?}", result.outcome);
            let so = String::from_utf8_lossy(&result.stdout);
            let se = String::from_utf8_lossy(&result.stderr);
            if !so.trim().is_empty() {
                eprintln!("--- stdout ---\n{}", so);
            }
            if !se.trim().is_empty() {
                eprintln!("--- stderr ---\n{}", se);
            }
        }
        Err(e) => {
            eprintln!("run error: {e:?}");
            std::process::exit(1);
        }
    }
}
