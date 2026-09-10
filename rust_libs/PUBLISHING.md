# 发布到 PyPI

四个包用 **PyPI API token** 发布。发布由 `.github/workflows/publish-pypi.yml`
手动触发，默认指向 TestPyPI。

## 一次性配置

仓库侧的两个 GitHub Environment（`testpypi`、`pypi`）已经建好，无需再操作。
若希望正式发布前需人工点确认，可在 Settings → Environments → `pypi` 里加上
required reviewers——发布不可逆，加一道确认是划算的。

需要你做的只有一件事——生成两个 token 并各自存进对应的 Environment：

1. <https://pypi.org/manage/account/token/> → Add API token → 作用域选
   **Entire account**（此时四个项目还不存在，选不了按项目限定的作用域；
   等四个包都发布过一次后，可以回来重新生成四个各自限定项目的 token 替换掉，
   降低单个 token 的影响面）。
2. 仓库 → Settings → Environments → `pypi` → Environment secrets →
   New secret，名字填 `PYPI_API_TOKEN`，值粘贴刚生成的 token。
3. 到 <https://test.pypi.org/manage/account/token/> 同样生成一个（TestPyPI
   是完全独立的账号体系，token 不通用），存进 Environment `testpypi`，
   名字同样是 `PYPI_API_TOKEN`。

两个 Environment 下用的是**同一个 secret 名字、不同的值**——workflow 里
只写 `secrets.PYPI_API_TOKEN` 一处引用，实际取到哪个由本次选择的
`environment: name` 决定，不需要条件判断。

**token 生成后只应贴入 GitHub 的 secret 输入框，不要贴进任何聊天、issue
或提交里**——那些地方的内容可能被记录留存，一旦贴出就应视为已泄露，需要
立刻去 PyPI 撤销重新生成。

（此前这里写的是 Trusted Publishing／OIDC 方案：不存密钥、每次发布临时换取
令牌、但要为四个包各填一次网页表单。两条路都能用，token 方案配置更快，
代价是多一个需要自己保管的长期密钥；如果之后想换回去，把
`publish-pypi.yml` 的 `publish` job 换成 `permissions: id-token: write`
并去掉 `password` 参数即可。）

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
