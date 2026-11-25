# 快速开始

在公网服务器启动 OPENPPP2，花 3 分钟让本机的第一条 HTTPS 请求经它访问互联网。

已有订阅链接可直接[导入远程订阅](../guides/REMOTE_SUBSCRIPTION_CN.md)。已有客户端配置可跳到[第 3 步](#start)并选择「客户端」，Windows 用户也可用[桌面客户端](DESKTOP_CLIENT_CN.md)从源码运行图形界面。
本例使用普通用户权限，只让指定应用走隧道。要给浏览器设置代理，见[仅开本地代理模式](../guides/PROXY_MODE_CN.md)。

!!! note "开始前准备"
    准备一台有公网 IPv4 的服务器，在云安全组和主机防火墙放行入站 **TCP 20000**。本例用 TCP 承载，无需开放 UDP。服务端需要 root 或管理员权限。本地代理客户端用普通终端即可。接管整机路由和 DNS 的 TUN 客户端需要 root 或管理员权限。

## 1. 下载与准备

服务端和客户端使用同一版本的 `ppp`。在两台机器上分别选择适合本机的包，下载到空目录，然后在该目录打开终端。[下载发行包](https://github.com/Miaocchi/openppp2/releases)

=== "Linux"

    运行 `uname -m` 查看架构。`x86_64` 选通用包 `openppp2-linux-amd64.zip`。若显示 `aarch64`，把下面的文件名换成 `openppp2-linux-aarch64-cross.zip`。

    ```bash
    unzip openppp2-linux-amd64.zip
    chmod +x ./ppp
    ```

=== "Windows"

    x64 选 `openppp2-windows-x64-Release.zip`。ARM64 选 `openppp2-windows-ARM64-Release.zip`，并替换命令中的文件名。在 PowerShell 中完整解压，保留随包的 DLL 和 `Driver` 目录。

    ```powershell
    Expand-Archive openppp2-windows-x64-Release.zip -DestinationPath openppp2
    Set-Location openppp2
    ```

=== "macOS"

    Apple 芯片选 `openppp2-darwin-arm64.zip`。Intel 芯片选 `openppp2-darwin-x86_64.zip`，并替换命令中的文件名。

    ```bash
    unzip openppp2-darwin-arm64.zip
    chmod +x ./ppp
    ```

## 2. 编写最小配置

在服务器的程序旁保存 `server.json`，在本机的程序旁保存 `client.json`。先用密码管理器生成两个不同的随机密钥，分别替换服务端的两个占位值，再把填好的 `key` 整段复制给客户端。装有 OpenSSL 时也可以运行两次 `openssl rand -base64 24` 来生成它们。

=== "服务端"

    ```json hl_lines="3-5 7"
    {
      "key": {
        "protocol-key": "REPLACE_WITH_PROTOCOL_SECRET",
        "transport-key": "REPLACE_WITH_TRANSPORT_SECRET",
        "plaintext": false
      },
      "tcp": { "listen": { "port": 20000 } }
    }
    ```

=== "客户端"

    ```json hl_lines="3-5 9"
    {
      "key": {
        "protocol-key": "REPLACE_WITH_PROTOCOL_SECRET",
        "transport-key": "REPLACE_WITH_TRANSPORT_SECRET",
        "plaintext": false
      },
      "client": {
        "guid": "{REPLACE_WITH_CLIENT_UUID}",
        "server": "ppp://203.0.113.10:20000/"
      }
    }
    ```

    把 `203.0.113.10` 换成服务器的公网 IPv4。每台客户端生成自己的 GUID，替换花括号中的占位文字。Linux / macOS 用 `uuidgen`，Windows PowerShell 用 `[guid]::NewGuid()`。GUID 只填在客户端。

!!! tip "两端哪些值要对齐？"
    - **端口**。`ppp://` 使用 TCP。地址末尾的 `20000` 对应服务端的 `tcp.listen.port`。换端口时也要更新入站防火墙规则。
    - **密钥**。`protocol-key` 用于协议层，`transport-key` 用于传输层。每个字段的值都要和另一端的同名字段逐字一致。
    - **加密方式**。两端都写 `plaintext: false` 以启用加密。省略时默认为 `true`。这里两端都使用默认的 `key.protocol`（`aes-128-cfb`）和 `key.transport`（`aes-256-cfb`）。其余 `key` 字段先都省略，避免单边改动影响握手。

## 3. 启动并验证连通 {#start}

先启动服务端，再启动客户端。分别在解压目录运行适合本机的命令，保持两个终端打开。

=== "服务端"

    在 Linux 或 macOS 终端运行

    ```bash
    sudo ./ppp --mode=server --config=./server.json
    ```

    在 Windows 管理员 PowerShell 中运行

    ```powershell
    .\ppp.exe --mode=server --config=.\server.json
    ```

=== "客户端"

    在 Linux 或 macOS 终端运行

    ```bash
    ./ppp --mode=proxy --proxy-subnet=no --config=./client.json
    ```

    在 Windows 普通 PowerShell 中运行

    ```powershell
    .\ppp.exe --mode=proxy --proxy-subnet=no --config=.\client.json
    ```

    `--proxy-subnet=no` 跳过虚拟子网，只用本机代理访问互联网。SOCKS5 默认监听 `127.0.0.1:1080`，HTTP 代理监听 `127.0.0.1:8080`。

在两端 **PPP 的交互终端**输入 `openppp2 info`。客户端的 `Phase` 应为 `Connected`。第一台客户端接入后，服务端的 `Sessions` 应从 `0` 变为 `1`。

**验证请求的出口。** 在每台机器上另开一个终端，选择该机器对应的命令。

=== "Linux / macOS"

    在服务器上查询公网出口 IPv4，记下返回的地址。

    ```bash
    curl -4fsS --max-time 15 --noproxy '*' https://api.ipify.org
    ```

    在本机通过 SOCKS5 代理查询出口。

    ```bash
    curl -fsS --max-time 15 --noproxy localhost --proxy socks5h://127.0.0.1:1080 https://api.ipify.org
    ```

=== "Windows"

    在服务器上查询公网出口 IPv4，记下返回的地址。

    ```powershell
    curl.exe -4fsS --max-time 15 --noproxy '*' https://api.ipify.org
    ```

    在本机通过 SOCKS5 代理查询出口。

    ```powershell
    curl.exe -fsS --max-time 15 --noproxy localhost --proxy socks5h://127.0.0.1:1080 https://api.ipify.org
    ```

本机返回与服务器相同的公网出口 IPv4，就跑通了第一条请求。若 `Sessions` 一直为 `0`，先核对服务器地址、入站 TCP 端口和两端的 `key`。若会话已建立但请求失败，先确认服务器自身能完成第一条请求，再检查本机的代理端口。[运维与排障](../operations/OPERATIONS_CN.md) · [错误码](../reference/ERROR_CODES_CN.md)

## 下一步

<div class="grid cards" markdown>

- **[编写配置文件](CONFIGURE_CN.md)**

    要换监听端口、改用 WebSocket 或增加多条链路，从一份能连通的配置开始。

- **[用户手册](USER_MANUAL_CN.md)**

    要让整机流量走隧道，按 TUN 客户端的权限和启动步骤创建虚拟网卡。

- **[路由与 DNS](../guides/ROUTING_AND_DNS_CN.md) · [Policy CLI](../guides/POLICY_CLI_CN.md)**

    选择哪些目标直连、哪些经过隧道，并检查规则是否命中。

- **[部署](../operations/DEPLOYMENT_CN.md)**

    把前台运行的服务端交给系统服务管理，设置开机启动。

- **[平台说明](../guides/PLATFORMS_CN.md)**

    查看桌面系统的网卡要求，或了解 Android 和 iOS 的 VPN 授权。

- **[配置字段](../reference/README_CN.md) · [命令行参数](../reference/CLI_REFERENCE_CN.md)**

    按名称查一个选项的默认值、取值范围和生效条件。

</div>
