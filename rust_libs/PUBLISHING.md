# 发布到 PyPI

四个包用 **Trusted Publishing（OIDC）** 发布，不在仓库 secrets 里存 API token。
发布由 `.github/workflows/publish-pypi.yml` 手动触发，默认指向 TestPyPI。

## 一次性配置

PyPI 上每个包是独立项目，**四个都要各配一次**。项目尚不存在时，在
<https://pypi.org/manage/account/publishing/> 用「pending publisher」预先登记即可。

| 字段 | 值 |
|---|---|
| PyPI Project Name | `j-gif` / `j-stitch` / `j-clipboard` / `j-ppocr` |
| Owner | `1003129155` |
| Repository name | `jietuba` |
| Workflow name | `publish-pypi.yml` |
| Environment name | `pypi`（TestPyPI 上填 `testpypi`） |

TestPyPI 需在 <https://test.pypi.org/manage/account/publishing/> 单独配一遍。

仓库侧还需建两个 GitHub Environment（Settings → Environments）：`testpypi` 与
`pypi`。给 `pypi` 加上 required reviewers，正式发布就需要人工点确认。

## 发布流程

1. 改版本号：`rust_libs/<crate>/Cargo.toml` 与 `pyproject.toml` 两处要一致
2. 本地跑一遍 `cargo about generate`，把更新后的 `THIRD-PARTY-NOTICES.txt` 提交
   （CI 会比对，过期即判红）
3. Actions → Publish to PyPI → Run workflow，先选 `testpypi`
4. 从 TestPyPI 装一遍验证：
   `pip install --index-url https://test.pypi.org/simple/ j-stitch`
5. 无误后再跑一次，选 `pypi`

## 不可逆的部分

- 包名一经注册即永久占用
- 版本号发出去撤不回，同一版本也**不能重传**——发错只能作废后发新版本
- 因此 workflow 里有 `twine check`：描述渲染失败一类的问题必须在上传前发现

## 重新生成第三方许可声明

```bash
cargo install cargo-about --locked --features cli
cd rust_libs
for c in gifrecorder longstitch ppocr_rust pyclipboard; do
  cargo about generate --manifest-path $c/Cargo.toml -o $c/THIRD-PARTY-NOTICES.txt notices.hbs
done
```

依赖树里出现新的许可证时 `cargo about` 会报错，需先在 `about.toml` 的
`accepted` 列表里确认并加入——这是有意的闸门，不要盲目加。
