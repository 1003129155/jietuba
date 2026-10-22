use std::{env, fs, path::PathBuf};

const MANIFEST: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
</assembly>
"#;

// 给更新器 exe 写入图标、版本信息和 manifest，缺少这些资源的小型 exe 更容易被启发式扫描误判。
fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=../../app.ico");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }

    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    // 图标沿用主程序的，复制到 OUT_DIR 与生成的 rc 脚本放在一起。
    fs::copy("../../app.ico", out.join("app.ico")).unwrap();
    fs::write(out.join("manifest.xml"), MANIFEST).unwrap();

    let version = env::var("CARGO_PKG_VERSION").unwrap();
    let mut numbers: Vec<u32> = version.split('.').map(|part| part.parse().unwrap()).collect();
    numbers.resize(4, 0);
    let numbers = format!("{},{},{},{}", numbers[0], numbers[1], numbers[2], numbers[3]);
    let resource = format!(
        r#"1 ICON "app.ico"
1 24 "manifest.xml"
1 VERSIONINFO
FILEVERSION {numbers}
PRODUCTVERSION {numbers}
FILEFLAGSMASK 0x3f
FILEFLAGS 0
FILEOS 0x40004
FILETYPE 1
FILESUBTYPE 0
{{
  BLOCK "StringFileInfo"
  {{
    BLOCK "040904B0"
    {{
      VALUE "ProductName", "jietuba"
      VALUE "ProductVersion", "{version}"
      VALUE "FileVersion", "{version}"
      VALUE "FileDescription", "jietuba updater"
      VALUE "InternalName", "jietuba_updater"
      VALUE "OriginalFilename", "jietuba_updater.exe"
      VALUE "LegalCopyright", "Copyright (c) 2025 JYAARU"
    }}
  }}
  BLOCK "VarFileInfo"
  {{
    VALUE "Translation", 0x0409, 1200
  }}
}}
"#
    );
    let script = out.join("resource.rc");
    fs::write(&script, resource).unwrap();
    embed_resource::compile(script, embed_resource::NONE).manifest_required().unwrap();
}
