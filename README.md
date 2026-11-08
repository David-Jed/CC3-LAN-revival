# lanfix: fixing LAN play in Red Alert 3, C&C3 and Kane's Wrath

**lanfix 1.2.0** is a small drop-in DLL for SAGE-engine Command & Conquer games. It fixes LAN
games where the other player never shows up, the host doesn't respond, or you appear twice
in the lobby. This happens on PCs with VPN or virtual network adapters.

No game files are modified. Remove one file and the game is exactly as it was.

> **TL;DR:** If you have VirtualBox, Hyper-V, WSL, Docker or a VPN on your PC, the game
> can pick the wrong network address for itself and announce an address the other PC can't
> reach. Copy `dsound.dll` and `lanfix.ini` next to the game's executable on every PC that
> plays.

---

## Symptoms

- Two legitimate copies on the same home network can't see each other in the LAN lobby.
- One player sees the other's game, but joining times out ("host not responding").
- Your own name appears twice in the lobby.
- Turning off VPNs doesn't help.
- It used to work, then stopped after installing VirtualBox, Docker, WSL or a VPN client,
  or after the router gave your PC a new address.

## What's actually going wrong

The game's networking isn't broken. The problem is how it picks **its own address**: the
address it binds its LAN sockets to and tells other players to connect to. The game was
written for PCs with one network card, and modern PCs often have several. We found three
ways it goes wrong.

### 1. It takes the first address Windows lists

When no address is saved in the profile, the game calls `gethostname()` and
`gethostbyname()` and takes the first result. On the PC we investigated, Windows listed
the **VirtualBox host-only adapter (192.168.56.1)** first, ahead of the real Ethernet
address. That address only exists inside the PC, so the other player could never reach it.

VirtualBox, VMware, Hyper-V, WSL and Docker all add adapters like this, so turning off
your VPN doesn't help.

### 2. It asks Windows "which address reaches the outside?"

In Red Alert 3 1.13, the routine at `0x00AF93A0` works out the local address by opening
a UDP socket, calling `connect()`, and reading the address back with `getsockname()`.
With a VPN connected, Windows answers with the **VPN tunnel address** (NordLynx, 10.5.0.2
in our case). The game then binds its LAN socket to the VPN address. Broadcasts from the
other player arrive on Ethernet, so the game never hears them.

The same routine also has a `SIO_ROUTING_INTERFACE_QUERY` path. RA3 never takes it,
because it asks for Winsock 2.0 and the version check expects at least `0x200`.

### 3. LAN discovery broadcasts follow the VPN

The game finds LAN games by broadcasting to `255.255.255.255` on UDP ports
**8086–8088**. With a VPN up, Windows can send those broadcasts down the tunnel instead
of out to the home network.

A related trap: the game saves the chosen address in
`%APPDATA%\Red Alert 3\Profiles\<profile>\Options.ini` (`IPAddress`, `GameSpyIPAddress`).
If the router later hands the PC a different address, the saved one goes out of date and
LAN play breaks again.

## How it was investigated

1. **Identify the binary.** The Steam launcher `RA3.exe` runs `Data\ra3_1.13.game`, a
   32-bit, unpacked MSVC 2005 build. The 2025 update (1.13) added Steam and Workshop
   support but left the GameSpy-era network code unchanged.
2. **Rule out the server side.** Online play still points at GameSpy hosts (`peerchat`,
   `gpcm`, `natneg1-3`, `master`), which no longer exist in DNS. LAN play needs no servers,
   so it should work.
3. **Rule out the firewall.** Inbound allow rules for the game covered TCP and UDP on the
   active network profile.
4. **Reproduce the address choice.** Running the same `gethostbyname` lookup the game uses
   returned the VirtualBox address first. Setting `IPAddress` in Options.ini to the real
   address fixed LAN play, which confirmed the cause.
5. **Retest with a VPN.** With NordVPN connected the other player vanished again. The
   game's LAN socket was bound to `10.5.0.2:8086`. Disassembling the address routine
   showed the `connect()`/`getsockname()` method described above.

## What lanfix does

lanfix is a stand-in `dsound.dll` that sits next to the game executable. It passes every
sound call straight through to Windows' real `dsound.dll`. While the game runs, it
redirects five of the game's network calls (in the game's import table, in memory only):

| Game call | lanfix change |
|---|---|
| `gethostbyname(<this PC>)` | Lists the chosen adapter's address first |
| `connect()` on an unbound UDP socket | Binds it to the chosen adapter first, so `getsockname()` reports the right address |
| `bind()` to any other specific address | Moves it to the chosen adapter (covers VPN and out-of-date addresses) |
| `sendto(255.255.255.255)` | Sends to the chosen adapter's subnet broadcast instead (e.g. `192.168.0.255`) |
| `WSAIoctl` route and interface queries | Answers with the chosen adapter |

**Choosing the adapter.** With `Adapter=auto`, lanfix picks the adapter that is up, has a
normal IPv4 address, and scores best on:
- not being a virtual or VPN adapter (judged by its name and description)
- being Ethernet or Wi-Fi
- having a gateway
- having a private address

Every adapter, its score and the final choice are written to `lanfix.log`.

## Installation

1. Download `lanfix-1.2.0.zip` and check it against the checksum below.
2. Copy **`dsound.dll`** and **`lanfix.ini`** into the folder that holds the game's
   executable, not the main game folder:

   | Game | Folder |
   |---|---|
   | Red Alert 3 | `...\Command and Conquer Red Alert 3\Data\` |
   | Command & Conquer 3 | `...\Command and Conquer 3 Tiberium Wars\RetailExe\1.10\` |
   | Kane's Wrath | `...\Command and Conquer 3 - Kane's Wrath\RetailExe\1.3\` |

   Steam's default location is `C:\Program Files (x86)\Steam\steamapps\common\`.

3. Install it on **every PC that plays**. On a PC that wasn't affected it changes nothing.
4. Check it worked: start the game, open the LAN lobby, quit, and open `lanfix.log` in the
   same folder. The `chosen adapter:` line should show your home-network address, for
   example `192.168.0.x` or `192.168.1.x`.

**Uninstall:** delete `dsound.dll`, `lanfix.ini` and `lanfix.log`. To switch it off without
deleting anything, set `Enabled=0` in `lanfix.ini`.

### Settings (`lanfix.ini`)

| Setting | Meaning |
|---|---|
| `Enabled=1` | `0` turns lanfix off; sound still passes through |
| `Adapter=auto` | Pick the real home-network adapter automatically |
| `Adapter=192.168.1.20` | Use the adapter with this address |
| `Adapter=ZeroTier` | Use the first adapter whose name contains this text. Use this for LAN play over ZeroTier, Radmin VPN or Hamachi; both players must choose that adapter |
| `FixBroadcast=1` | Keep LAN discovery on the chosen network while a VPN is on |
| `FixBind=1` | Keep the game on the chosen adapter when it asks for another address |
| `Log=1` | Write `lanfix.log` |

## Troubleshooting

- **Still can't see the other player:** check that both PCs have lanfix and that both
  `lanfix.log` files show addresses on the same network. Some VPNs block all local
  traffic. In NordVPN, turn on **Allow LAN discovery** (or turn off **Invisibility on
  LAN**). Other VPNs have a similar "allow local network" option.
- **Wrong adapter chosen:** find the one you want in `lanfix.log` and put its address, or
  part of its name, in `Adapter=`.
- **You appear twice in the lobby:** leave the lobby and rejoin (see Known issues).
- **Game won't start, or there's no sound:** delete `dsound.dll` and report the problem
  with your `lanfix.log`.
- **Antivirus warning:** lanfix is a new, unsigned DLL that loads into the game, so some
  antivirus products block or quarantine it. Bitdefender did in testing. Only allow it if
  you got it from a source you trust.

## Known issues and limitations

- **VPN address still detected.** With a VPN connected, the game still works out the VPN
  address as its own through a path that hasn't been identified yet. lanfix moves the
  game's socket to the right adapter, which is enough for LAN play. But the game may still
  advertise the VPN address, and in some sessions your own name shows up twice. Rejoining
  the lobby, or disconnecting the VPN, clears it.
- **Only Red Alert 3 tested.** lanfix was run in Red Alert 3 (Steam, 1.13) on Windows 11.
  C&C3 1.10 and Kane's Wrath 1.3 import the same network and sound functions, so they're
  expected to work, but they haven't been run with lanfix.
- **Online play isn't fixed.** GameSpy's servers are gone; community servers such as
  C&C:Online are a separate project. If you play there with a VPN on, lanfix sends the
  game's traffic over your normal connection instead of the VPN. Set `Enabled=0` if you
  need the VPN for online play.

## How it was tested

- **Test harness.** A 32-bit test program imports the same network and sound functions the
  games use. It was run under five configurations with a VPN connected: lanfix off, `auto`,
  an adapter name that matches nothing, a deliberate override to the VirtualBox adapter,
  and broadcast/bind fixes off. Each hook was checked against the baseline.
- **In-game.** Red Alert 3 1.13 was tested between two PCs on a home network, with
  VirtualBox installed and NordVPN (NordLynx) connected. LAN discovery and joining worked
  with no addresses saved in Options.ini.

## Building from source

Requirements: Visual Studio with the C++ x86 tools.

```bat
build.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File test\run-tests.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
```

`package.ps1` builds the DLL and creates `dist\lanfix-<version>\` and the release zip, with
`SHA256SUMS.txt` inside.

## Checksums (1.2.0)

```
lanfix-1.2.0.zip  ce8a234d6c7058191fb47ce75733526afe6536eb9032058039c0168c843702ea
```

The per-file hashes are in `SHA256SUMS.txt` inside the zip.

---

*lanfix is an unofficial community fix. It isn't affiliated with or endorsed by Electronic
Arts. Command & Conquer, Red Alert and related names are trademarks of their owners. Use it
only with legitimately owned copies.*
