# Getting started

Make your first connection in three minutes. Run OPENPPP2 on a public server and send an HTTPS request through it from your computer.

Already have a subscription? [Import the subscription URL](../guides/REMOTE_SUBSCRIPTION.md). If you have a client configuration, go to [step 3](#start) and select **Client**, or use the [Windows desktop client](DESKTOP_CLIENT.md), which runs from source.
This walkthrough uses a local proxy with a regular user account. To point a browser at it, see [proxy-only mode](../guides/PROXY_MODE.md).

!!! note "Before you start"
    You need a server with a public IPv4 address. Allow inbound **TCP 20000** in both the cloud security group and the host firewall. This example uses TCP only. The server requires root or Administrator privileges. A local proxy works in a regular terminal. A TUN client needs root or Administrator privileges to change system routes and DNS.

## 1. Download and unpack

Use the same release of `ppp` on both computers. On each machine, download the package for its architecture into an empty directory and open a terminal there. [Download a release](https://github.com/Miaocchi/openppp2/releases)

=== "Linux"

    Check the architecture with `uname -m`. For `x86_64`, use the compatibility package `openppp2-linux-amd64.zip`. For `aarch64`, substitute `openppp2-linux-aarch64-cross.zip` in the command below.

    ```bash
    unzip openppp2-linux-amd64.zip
    chmod +x ./ppp
    ```

=== "Windows"

    Use `openppp2-windows-x64-Release.zip` on x64. On ARM64, substitute `openppp2-windows-ARM64-Release.zip`. Extract the entire archive in PowerShell so the DLLs and the `Driver` directory stay alongside the executable.

    ```powershell
    Expand-Archive openppp2-windows-x64-Release.zip -DestinationPath openppp2
    Set-Location openppp2
    ```

=== "macOS"

    Use `openppp2-darwin-arm64.zip` on Apple silicon. On an Intel Mac, substitute `openppp2-darwin-x86_64.zip` in the command below.

    ```bash
    unzip openppp2-darwin-arm64.zip
    chmod +x ./ppp
    ```

## 2. Write the minimal configuration

Save `server.json` beside the executable on the server and `client.json` beside the executable on your computer. Generate two different random secrets with a password manager. Replace both placeholders on the server, then copy the completed `key` block to the client. If you have OpenSSL, you can generate the secrets by running `openssl rand -base64 24` twice.

=== "Server"

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

=== "Client"

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

    Replace `203.0.113.10` with the server's public IPv4 address. Generate a GUID for each client with `uuidgen` on Linux or macOS, or `[guid]::NewGuid()` in Windows PowerShell. Replace the text inside the braces. Only the client needs this ID.

!!! tip "Which settings must match?"
    - **Port.** The `ppp://` URL uses TCP. Its port (`20000`) must match `tcp.listen.port` on the server. If you change it, update the inbound firewall rules too.
    - **Secrets.** `protocol-key` is the protocol-layer secret and `transport-key` is the transport-layer secret. Each value must match its counterpart on the other machine exactly.
    - **Encryption.** Set `plaintext: false` on both ends to enable encryption. It defaults to `true` if omitted. Both ends use the default `key.protocol` (`aes-128-cfb`) and `key.transport` (`aes-256-cfb`). Leave the remaining `key` fields out on both ends for this first handshake.

## 3. Start and verify the connection {#start}

Start the server first, then the client. Run the command for each machine from its extracted directory and leave both terminals open.

=== "Server"

    On Linux or macOS, run

    ```bash
    sudo ./ppp --mode=server --config=./server.json
    ```

    In an Administrator PowerShell on Windows, run

    ```powershell
    .\ppp.exe --mode=server --config=.\server.json
    ```

=== "Client"

    On Linux or macOS, run

    ```bash
    ./ppp --mode=proxy --proxy-subnet=no --config=./client.json
    ```

    In a regular PowerShell on Windows, run

    ```powershell
    .\ppp.exe --mode=proxy --proxy-subnet=no --config=.\client.json
    ```

    `--proxy-subnet=no` skips joining the virtual subnet. Applications can reach the internet through SOCKS5 at `127.0.0.1:1080` or HTTP at `127.0.0.1:8080`.

Type `openppp2 info` in each **interactive PPP terminal**. On the client, `Phase` should read `Connected`. On the server, `Sessions` should rise from `0` to `1` when the first client connects.

**Check where the request exits.** Open a second terminal on each machine. Select the commands for its operating system.

=== "Linux / macOS"

    On the server, record the public egress IPv4 address.

    ```bash
    curl -4fsS --max-time 15 --noproxy '*' https://api.ipify.org
    ```

    On your computer, query the address through the SOCKS5 proxy.

    ```bash
    curl -fsS --max-time 15 --noproxy localhost --proxy socks5h://127.0.0.1:1080 https://api.ipify.org
    ```

=== "Windows"

    On the server, record the public egress IPv4 address.

    ```powershell
    curl.exe -4fsS --max-time 15 --noproxy '*' https://api.ipify.org
    ```

    On your computer, query the address through the SOCKS5 proxy.

    ```powershell
    curl.exe -fsS --max-time 15 --noproxy localhost --proxy socks5h://127.0.0.1:1080 https://api.ipify.org
    ```

The client should return the server's public egress IPv4 address. If `Sessions` stays at `0`, check the server address, inbound TCP port and both copies of `key`. If the session is up but the request fails, check that the first command works on the server, then check the client's proxy port. [Troubleshooting](../operations/OPERATIONS.md) · [Error codes](../reference/ERROR_CODES.md)

## Next steps

<div class="grid cards" markdown>

- **[Write a configuration file](CONFIGURE.md)**

    Change the listening port, switch to WebSocket or add another link to your working configuration.

- **[User manual](USER_MANUAL.md)**

    Route traffic from the whole computer through the tunnel. Follow the TUN client instructions for privileges and virtual adapter setup.

- **[Routing and DNS](../guides/ROUTING_AND_DNS.md) · [Policy CLI](../guides/POLICY_CLI.md)**

    Choose which destinations connect directly and which use the tunnel, then check which rule matches.

- **[Deployment](../operations/DEPLOYMENT.md)**

    Run the server as a system service and start it at boot.

- **[Platform notes](../guides/PLATFORMS.md)**

    Look up desktop adapter requirements or VPN permissions on Android and iOS.

- **[Configuration fields](../reference/README.md) · [CLI flags](../reference/CLI_REFERENCE.md)**

    Look up a setting by name to find its default, accepted values and when it takes effect.

</div>
