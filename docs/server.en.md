[Русский](server.md) | **English**

# Server for playing over the internet (experimental)

SolarNX Rendezvous is a small server you run on your own VPS (or any machine with a public IP
address). With it, Internet mode works **without port forwarding**: the server introduces the PC and
the Switch to each other, and from then on they talk directly.

How it works:

1. In Internet mode, the PC registers on the server under its **ID** (8 characters, shown in the PC's
   settings).
2. The Switch asks the server for the PC's address by that ID. The server tells the Switch the PC's
   public address and tells the PC the Switch's.
3. Both sides start sending to each other at the same moment, and the routers let that traffic through
   (so-called NAT hole punching). From then on video, audio and input go directly, bypassing the server.
4. If a direct connection can't be made (for example, one side is behind a provider's shared address),
   the server can forward the traffic itself, if you allow it.
5. If the PC and the Switch are behind the same router, the server gives the Switch the PC's local
   address, and the traffic stays on the home network.

All traffic between the PC and the Switch is **encrypted** before it reaches the server: the server sees
only addresses and IDs, not the game, the input or the keys. Only a Switch paired with the PC can
connect to it.

## Requirements

- A VPS or another machine with a public IPv4 address: Linux (x64) or Windows (x64).
- **UDP port 19020** open in its firewall (can be changed).
- For relaying: bandwidth for a video stream, about 4–20 Mbit/s per player.

## Installation

### Ready-made program

Download `SolarNX-Rendezvous-linux-x64.zip` or `SolarNX-Rendezvous-win-x64.zip` from the
[releases](https://github.com/HaDoKit/SolarNX/releases) and unpack it. The program is self-contained:
no separate .NET installation is needed.

```
./solarnx-rendezvous --key my-secret-key
```

To start the server with the system on Linux, use the service file from the archive:

```
sudo mkdir -p /opt/solarnx-rendezvous && sudo cp * /opt/solarnx-rendezvous/
sudo chmod +x /opt/solarnx-rendezvous/solarnx-rendezvous
sudo cp solarnx-rendezvous.service /etc/systemd/system/
sudo systemctl enable --now solarnx-rendezvous
journalctl -u solarnx-rendezvous -f      # log
```

### Docker

Copy the repository's `Server` folder to the server (that folder alone is enough) and build the image
in it:

```
cd Server
sudo docker build -t solarnx-rendezvous .
sudo docker run -d --name solarnx-rendezvous --restart unless-stopped -p 19020:19020/udp \
  -e SOLARNX_RV_ACCESS_KEY=my-secret-key solarnx-rendezvous
sudo docker logs -f solarnx-rendezvous      # log
```

Relaying is turned on with `-e SOLARNX_RV_RELAY=true`.

## Settings

Set them in `rendezvous.json` next to the program (sample: `rendezvous.example.json`), in environment
variables or as command-line options. Command-line options win over variables, variables over the file.

| File | Variable | Option | Default | What it does |
|---|---|---|---|---|
| `Port` | `SOLARNX_RV_PORT` | `--port` | 19020 | the server's UDP port |
| `AccessKey` | `SOLARNX_RV_ACCESS_KEY` | `--key` | none | the key the PC and the Switch must enter. Without a key anyone can use the server |
| `Relay` | `SOLARNX_RV_RELAY` | `--relay` / `--no-relay` | off | forward traffic when a direct connection fails |
| `RelayMaxKbps` | `SOLARNX_RV_RELAY_MAX_KBPS` | `--relay-max-kbps` | 20000 | relay limit per Switch, kbit/s |
| `MaxHosts` | `SOLARNX_RV_MAX_HOSTS` | `--max-hosts` | 64 | how many PCs may be registered at once |
| `HostTimeoutSeconds` | `SOLARNX_RV_HOST_TIMEOUT` | `--host-timeout` | 60 | forget a PC after this many silent seconds |
| `Verbose` | `SOLARNX_RV_VERBOSE` | `--verbose` | off | log every request |

`--config <file>` reads the settings from another file, `--help` lists the options.

> Always set an access key: without one, other people can use your server. With relaying on, that's
> your traffic too.

## Connecting the PC and the Switch

1. **PC:** «Настройки» (Settings) → «Подключение» (Connection) → mode **«Интернет
   (экспериментально)»** (Internet, experimental). In **«Сервер-посредник»** (Rendezvous server) enter
   the server address (`example.com` or `example.com:19020`), in **«Ключ сервера»** (Server key) the
   key. Note the **«ID этого ПК»** (This PC's ID). Turn streaming on; the PC registers on the server.
2. **Switch:** main menu → **X** → Connection: **Mode** Internet, **Server** the server address,
   **Server key** the key, **PC ID** the ID from the PC. Restart SolarNX on the Switch.
3. **Pairing** (once): «Привязать Switch…» (Pair Switch) on the PC, "Pair with this PC" on the Switch,
   compare the 6-digit code. After pairing the PC ID is remembered by itself.

No port forwarding on the PC's router is needed in this setup. The Switch's "PC address" field isn't
used while a server is set.

## If it doesn't connect

The "X  Internet: …" line in the Switch menu tells why:

| Message | What to do |
|---|---|
| server address not found | check the server address |
| the server doesn't answer | the server isn't running, or its UDP port is closed in the VPS firewall |
| the server refused the key | the key on the Switch doesn't match the server's |
| PC not on the server | streaming isn't on in Internet mode on the PC, the PC uses another server or key, or the PC ID is wrong |
| waiting for the PC | the server found the PC but no connection could be made: turn relaying on (`--relay`) |

The server's log shows which PCs are registered and when relaying kicks in.
