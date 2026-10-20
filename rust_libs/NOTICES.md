# 第三方许可声明

C++ 发布包静态链接 rust_libs 的各个 crate，`cpp/scripts/build.ps1` 把每个 crate 的
`THIRD-PARTY-NOTICES.txt` 拷进包里。依赖变了就重新生成并提交，`.github/workflows/rust.yml` 会比对，过期即判红。

```bash
cargo install cargo-about --locked --features cli
cd rust_libs
for c in gifrecorder longstitch ppocr_rust pyclipboard hdrcapture; do
  cargo about generate --manifest-path $c/Cargo.toml -o $c/THIRD-PARTY-NOTICES.txt notices.hbs
done
# 更新器和录制库用自己的模板
cargo about generate --manifest-path updater/Cargo.toml -o updater/THIRD-PARTY-NOTICES.txt updater/notices.hbs
cargo about generate --locked --manifest-path video_recorder/Cargo.toml -o video_recorder/THIRD-PARTY-NOTICES.txt video_recorder/notices.hbs
```

依赖树里出现新的许可证时 `cargo about` 会报错，需先在 `about.toml` 的
`accepted` 列表里确认并加入——这是有意的闸门，不要盲目加。

`ppocr_rust` 静态链接的 ONNX Runtime 本体不在 crate 依赖树里，`cargo about` 不覆盖。
它的声明在 `ppocr_rust/ONNXRUNTIME-NOTICES.txt`：升级 `ort` 时按 ort-sys 的
`build/download/dist.txt` 查出 ONNX Runtime 版本，从 microsoft/onnxruntime 对应标签
取 `LICENSE` 和 `ThirdPartyNotices.txt` 更新。
