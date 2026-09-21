//! Per-agent Python activation. Never mutate the application's environment.
use serde_json::Value;
use std::collections::HashMap;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};

pub struct PythonVenv {
    root: PathBuf,
    bin: PathBuf,
}

pub fn load(
    data_dir: &Path,
    cwd: &Path,
    home: Option<&Path>,
) -> Result<Option<PythonVenv>, String> {
    let text = match fs::read_to_string(data_dir.join("settings.json")) {
        Ok(text) => text,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(err) => {
            return Err(format!(
                "Cannot read Python virtual environment settings: {err}"
            ))
        }
    };
    let settings: Value = serde_json::from_str(&text)
        .map_err(|err| format!("Cannot read Python virtual environment settings: {err}"))?;
    if settings
        .get("agentPythonVenvEnabled")
        .and_then(Value::as_bool)
        != Some(true)
    {
        return Ok(None);
    }
    let configured = settings
        .get("agentPythonVenvPath")
        .and_then(Value::as_str)
        .unwrap_or("")
        .trim();
    if configured.is_empty() {
        return Err(
            "Set the Python virtual environment path in Settings → Agent before starting an agent."
                .into(),
        );
    }
    let path = if let Some(relative) = configured
        .strip_prefix("~/")
        .or_else(|| configured.strip_prefix("~\\"))
    {
        home.ok_or("Cannot resolve the home directory for the Python virtual environment")?
            .join(relative)
    } else {
        cwd.join(configured)
    };
    let invalid =
        |reason: String| format!("Invalid Python virtual environment \"{configured}\": {reason}");
    let root = path
        .canonicalize()
        .map_err(|err| invalid(err.to_string()))?;
    if !root.join("pyvenv.cfg").is_file() {
        return Err(invalid("missing pyvenv.cfg".into()));
    }
    let bin = root.join(if cfg!(windows) { "Scripts" } else { "bin" });
    let python = bin.join(if cfg!(windows) {
        "python.exe"
    } else {
        "python"
    });
    if !python.is_file() {
        return Err(invalid("missing Python executable".into()));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if fs::metadata(&python)
            .map_err(|err| invalid(err.to_string()))?
            .permissions()
            .mode()
            & 0o111
            == 0
        {
            return Err(invalid("Python is not executable".into()));
        }
    }
    Ok(Some(PythonVenv { root, bin }))
}

impl PythonVenv {
    pub fn environment(&self, inherited_path: &OsStr) -> Result<HashMap<String, String>, String> {
        let entries =
            std::iter::once(self.bin.clone()).chain(std::env::split_paths(inherited_path));
        let path = std::env::join_paths(entries)
            .map_err(|err| format!("Invalid Python virtual environment PATH: {err}"))?;
        let path = path
            .into_string()
            .map_err(|_| "Python virtual environment PATH is not valid Unicode")?;
        let root = self
            .root
            .to_str()
            .ok_or("Python virtual environment path is not valid Unicode")?;
        Ok(HashMap::from([
            ("PATH".into(), path),
            ("VIRTUAL_ENV".into(), root.into()),
        ]))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn validates_and_activates_without_modifying_shared_environment() {
        let root = std::env::temp_dir().join(format!(
            "bat-venv-{}-{}",
            std::process::id(),
            rand::random::<u64>()
        ));
        let workspace = root.join("workspace with spaces");
        let venv = workspace.join(".venv");
        let bin = venv.join(if cfg!(windows) { "Scripts" } else { "bin" });
        fs::create_dir_all(&bin).unwrap();
        let settings = |enabled: bool, path: &str| {
            fs::write(
                root.join("settings.json"),
                json!({"agentPythonVenvEnabled": enabled, "agentPythonVenvPath": path}).to_string(),
            )
            .unwrap();
        };
        assert!(load(&root, &workspace, None).unwrap().is_none());
        settings(false, "missing");
        assert!(load(&root, &workspace, None).unwrap().is_none());
        settings(true, "");
        assert!(load(&root, &workspace, None).is_err());
        settings(true, ".venv");
        assert!(load(&root, &workspace, None).is_err());
        fs::write(venv.join("pyvenv.cfg"), "home = /python\n").unwrap();
        assert!(load(&root, &workspace, None).is_err());
        let python = bin.join(if cfg!(windows) {
            "python.exe"
        } else {
            "python"
        });
        fs::write(&python, "").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert!(load(&root, &workspace, None).is_err());
            fs::set_permissions(&python, fs::Permissions::from_mode(0o755)).unwrap();
        }
        let original_path = std::env::var_os("PATH");
        let env = load(&root, &workspace, None)
            .unwrap()
            .unwrap()
            .environment(OsStr::new("/original/bin"))
            .unwrap();
        assert_eq!(Path::new(&env["VIRTUAL_ENV"]), venv.canonicalize().unwrap());
        assert_eq!(
            std::env::split_paths(&env["PATH"]).next().unwrap(),
            bin.canonicalize().unwrap()
        );
        assert_eq!(std::env::var_os("PATH"), original_path);
        settings(true, venv.to_str().unwrap());
        assert!(load(&root, &workspace, None).unwrap().is_some());
        settings(true, "~/workspace with spaces/.venv");
        assert!(load(&root, &workspace, Some(&root)).unwrap().is_some());
        fs::remove_dir_all(root).unwrap();
    }
}
