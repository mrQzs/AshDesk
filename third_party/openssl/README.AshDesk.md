# OpenSSL（第三方）

- 版本：**3.5.9**（2026-09-29），3.5 是 LTS，官方支持到 2030-04-08。主机和客户端启动时在日志第一行写出实际加载的版本
- 许可：Apache 2.0（`LICENSE.txt`）
- 用途：DTLS 1.2 + PSK-ECDHE-ChaCha20-Poly1305（`proto/common/adp_dtls.c`）
- 为什么不再用 miniconda 自带的：版本、补丁、来源跟着 conda 环境走，发布包里带哪个 DLL 不确定；2026-11 核对时 conda 里是 3.6.5，
  而 3.6 已经不在官方支持列表里（支持中的只有 4.1、4.0、3.5）
- 为什么不用 4.x：4.0 / 4.1 都不是 LTS（分别支持到 2027-05、2027-11）；4.1 才有 DTLS 1.3，发布说明里还列着 DTLS 1.3 的已知问题，
  见 `docs/优化点/08-安全与身份.md` 8.7
- 仓库里放：`port/`（vcpkg 的 openssl 端口，取自基线里 3.5.4 那一版，只把版本号改成 3.5.9、源码包 SHA512 换成 3.5.9 的）、
  `SHA256SUMS`（这次构建的 DLL 哈希）、`SHA256SUMS-macos-arm64`（macOS 那份静态库的哈希）、本说明。`bin/`、`lib/`、`include/` 不进仓库
- 最低要 3.2：DTLS 收发用的 `BIO_new_bio_dgram_pair` 是 3.2 才有的，`CMakeLists.txt` 里 `find_package(OpenSSL 3.2 REQUIRED)`

## 什么时候要更新

OpenSSL 发安全公告时看受影响的版本范围：包含 3.5.9 就升到 3.5 的最新修订版（改 `port/vcpkg.json` 的版本号和 `port/portfile.cmake`
的 SHA512，按下面重新编，更新 `SHA256SUMS`、本说明和 `CMakeLists.txt` 里的 `ADRD_OPENSSL_VERSION`）。

## 取得

用 VS 2022 自带的 vcpkg（清单模式），带上仓库里的端口（overlay）：

```
vcpkg.json（放在任意一个空目录里，比如 build/vcpkg-ossl）:
{ "name": "adrd-openssl", "version": "0.1.0",
  "builtin-baseline": "9eec002df1cd109f8f104cd966a763daf9751489",
  "dependencies": [ "openssl" ] }

"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\vcpkg\vcpkg.exe" install --triplet x64-windows ^
  --overlay-ports=D:/Code/AshDesk/third_party/openssl/port ^
  --x-buildtrees-root=D:/Code/AshDesk/build/vcpkg/obt --x-packages-root=D:/Code/AshDesk/build/vcpkg/opk ^
  --x-install-root=D:/Code/AshDesk/build/vcpkg/oinst
```

产物（`build/vcpkg/oinst/x64-windows/`）拷到这里：

- `bin/libssl-3-x64.dll`、`bin/libcrypto-3-x64.dll`
- `lib/libssl.lib`、`lib/libcrypto.lib`
- `include/openssl/`

源码包：`https://github.com/openssl/openssl/archive/openssl-3.5.9.tar.gz`，SHA512
`ab29a6ed40dd8c74420b9bd19c5ac119b0f191aee72d555ba5a0601456155d39c809c77300d98c7e0bce607f32c1f7a83fc7f69f86b9909fe557bba3aa2e468f`。

- 构建时间约 3 分钟；vcpkg 自己下载 Perl 和 NASM
- CMake 配置时按 `SHA256SUMS` 核对两个 DLL、按头文件核对版本号，不符就停下；自己重新编出来的 DLL 哈希会不同（构建不可复现），
  确认是自己编的以后重新生成 `SHA256SUMS`：`sha256sum bin/libssl-3-x64.dll bin/libcrypto-3-x64.dll | sed 's/ \*/  /' > SHA256SUMS`
- `tools/package.sh` 打包时也按 `SHA256SUMS` 核对要放进包里的 DLL
- 以前用 conda 的 OpenSSL 配置过的构建目录：删掉 `build/CMakeCache.txt` 重新配置

## macOS（arm64）

同一个源码包（先按上面的 SHA512 核对），和端口里 Unix 那一支同样的选项，编成静态库直接放进本目录（包里不用另带 dylib）：

```
./Configure darwin64-arm64-cc no-shared enable-static-engine no-tests no-docs enable-ec_nistp_64_gcc_128 \
  --prefix=$HOME/AshDesk/third_party/openssl --openssldir=/etc/ssl --libdir=lib      # MACOSX_DEPLOYMENT_TARGET=13.0
make -j10 && make install_sw
shasum -a 256 lib/libssl.a lib/libcrypto.a > SHA256SUMS-macos-arm64                  # 在本目录里跑
```

- 2026-11-13 在 .3（M6、Xcode 26.3）上编，18 s；CMake 配置时按 `SHA256SUMS-macos-arm64` 核对两个 `.a`、按头文件核对版本号
- 端口的公共选项里还有 `enable-capieng`，那是 Windows 的 CryptoAPI 引擎，这里不要

## Linux（只给 Sanitizer 门禁用）

Ubuntu 24.04 自带的是 3.0，不够 3.2。同一个源码包编到 WSL 自己的目录里（`include/` 里有生成的平台配置头，不能和 Windows 共用本目录）：

```
./Configure linux-x86_64 no-shared enable-static-engine no-tests no-docs enable-ec_nistp_64_gcc_128 \
  --prefix=$HOME/.local/adrd-openssl-3.5.9 --openssldir=/etc/ssl --libdir=lib
make -j$(nproc) && make install_sw
```

`tools/run_sanitizers.sh` 看到 `~/.local/adrd-openssl-3.5.9` 就用它，没有就退回系统的（版本不够时配置那一步直接报错）。
