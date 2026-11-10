# GitHub Actions 发布客户端

用此目录的内容作为 GitHub 仓库的根目录。`.github/workflows/client.yml` 需位于仓库根目录下的 `.github/workflows/`。首次推送先在 Actions 检查分支构建。仓库的 Actions 策略需允许工作流运行，发布任务使用单独的 `contents: write` 权限和 GitHub 内置令牌。

更新 `CMakeLists.txt` 中的版本号，提交并推送。在准备发布的提交上创建与该版本一致的标签，例如：

```sh
git tag -a v1.1.2 -m 'IP Reporter v1.1.2'
git push origin v1.1.2
```

标签触发所有平台的编译、CTest 与 CPack。Linux 任务使用指定发行版容器生成兼容该发行版的二进制，Windows 与 macOS 任务使用相应架构的 GitHub Runner。版本标签必须和 CMake 项目版本完全相同。全部构建成功后，发布任务验证每个平台的包和校验文件，并将完整资产上传到草稿，随后发布。

发布版本应视为不可变。如果已有同名 Release，工作流退出。草稿上传失败时，在 GitHub 检查草稿并处置后再重试。已发布资产若需更正，使用新的版本号和标签。手动运行工作流只进行构建验证。正式发版后可在 **Releases** 下载安装包；**Actions** 中的运行工件供开发和检查使用。

服务端镜像通过 GitHub Releases 获取公开版本。私有仓库是否可镜像取决于服务端的访问配置，管理方式见服务端部署文档。

## 发布工件约定

`ci/platforms.json` 定义平台标识、Runner 和包格式。GitHub Release 中每个平台只有一个可被一键安装入口选中的 ZIP 或 TGZ；额外的 `.deb`、`.rpm` 可按需下载并通过包管理器安装。

`INSTALL-INDEX.json` 约定如下，实际哈希和大小由 CI 计算：

```json
{
  "schemaVersion": 1,
  "version": "1.1.2",
  "tag": "v1.1.2",
  "packages": [
    {
      "platform": "windows-x86_64",
      "file": "ip-reporter-1.1.2-windows-x86_64.zip",
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
      "size": 123456
    }
  ]
}
```

`SHA256SUMS.txt` 记录所有 ZIP、TGZ、DEB、RPM 和 `INSTALL-INDEX.json` 的 SHA-256。可在下载文件后使用 `sha256sum -c SHA256SUMS.txt` 校验。每个归档的首层目录名与文件名去掉扩展名后的名称一致，Linux 在首层目录下含 `usr/bin/` 和 `usr/share/`，Windows 和 macOS 含 `bin/` 和 `share/`。

GitHub 账号或组织应管控谁能推送发布标签，服务端只需读取 Release。校验清单用于确认镜像和安装设备得到同一份资产，源仓库、发布权限和 HTTPS 端点决定发布信任范围。GitHub 可用 Runner 与 Release 接口分别见 [GitHub Runner 文档](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)和 [GitHub Release 文档](https://docs.github.com/en/rest/releases/releases)。
