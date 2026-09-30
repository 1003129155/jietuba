fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    // 预编译的 onnxruntime 带着 DirectML 执行提供程序，静态导入了下面三个 DLL。推理只用 CPU，
    // 不会调用它们；改成延迟加载，系统里缺少 DirectML.dll 时模块照样能加载。
    let windows_msvc = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows")
        && std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");
    if windows_msvc {
        for dll in ["DirectML.dll", "d3d12.dll", "dxgi.dll"] {
            println!("cargo:rustc-link-arg-cdylib=/DELAYLOAD:{dll}");
        }
        println!("cargo:rustc-link-arg-cdylib=delayimp.lib");
    }
}
