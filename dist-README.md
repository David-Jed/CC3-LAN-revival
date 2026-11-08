lanfix 1.2.0 - LAN play fix for Command & Conquer 3, Kane's Wrath and Red Alert 3
====================================================================================

WHAT IT FIXES
-------------
LAN games where the other player never shows up, the host doesn't respond, or you
appear twice in the lobby. This happens when the PC has extra network adapters, so
the game picks the wrong address for itself and the other PC can't reach it:

  - VirtualBox, VMware, Hyper-V, WSL or Docker installed
  - A VPN running while you play (NordVPN, WireGuard, OpenVPN, ...)
  - The router has given your PC a new address since the game last saved one

lanfix makes the game use your real home-network adapter (Ethernet or Wi-Fi) for LAN
play. It changes no game files: it is a small dsound.dll that sits next to the game,
passes all sound through to Windows unchanged, and corrects the game's network calls
while it runs.

Online play is not affected by this fix (GameSpy's servers are gone; see C&C:Online).


INSTALL
-------
Copy dsound.dll and lanfix.ini into the folder that holds the game's executable -
not the main game folder:

  Red Alert 3      ...\Command and Conquer Red Alert 3\Data\
  C&C3             ...\Command and Conquer 3 Tiberium Wars\RetailExe\1.10\
  Kane's Wrath     ...\Command and Conquer 3 - Kane's Wrath\RetailExe\1.3\

(Steam default: C:\Program Files (x86)\Steam\steamapps\common\...)

Install it on every PC that plays. On a PC that wasn't affected it changes nothing.

Check it worked: start the game, open the LAN lobby, quit, and open lanfix.log in the
same folder. The first lines say which adapter was chosen.


UNINSTALL
---------
Delete dsound.dll, lanfix.ini and lanfix.log from that folder.
To switch it off without deleting: set Enabled=0 in lanfix.ini.


SETTINGS (lanfix.ini)
---------------------
  Adapter=auto       Default. Picks your real Ethernet/Wi-Fi adapter and skips VPN and
                     virtual adapters.
  Adapter=10.147.17.5  Use the adapter with this address.
  Adapter=ZeroTier   Use the first adapter whose name contains this text. Use this if
                     you play "LAN" over ZeroTier, Radmin VPN, Hamachi or similar on
                     purpose - both players must pick that adapter.
  FixBroadcast=1     Keep LAN game discovery on your home network while a VPN is on.
  FixBind=1          Make the game use the chosen adapter even when it asks for
                     another address (a VPN's, or an out-of-date one).
  Log=1              Write lanfix.log.


ANTIVIRUS
---------
lanfix is a new, unsigned DLL that loads into the game, so some antivirus products
block or quarantine it. If the game won't start, or lanfix.log never appears, check
your antivirus and allow dsound.dll in the game folder. Only do this if you got lanfix
from a source you trust.


TROUBLESHOOTING
---------------
- Still can't see the other player:
    * Make sure both PCs have lanfix and both lanfix.log files show an address on the
      same network (e.g. both 192.168.0.x).
    * Some VPNs block all local traffic. In NordVPN turn on "Allow LAN discovery"
      (or turn off "Invisibility on LAN"); other VPNs have a similar
      "allow local network" setting.
    * Allow the game through Windows Firewall on both PCs.
- You appear twice in the lobby: leave the lobby and rejoin. See KNOWN ISSUES.
- Wrong adapter chosen: open lanfix.log, find the adapter you want, and put its
  address or part of its name in Adapter= in lanfix.ini.
- Game won't start or has no sound: delete dsound.dll and report it with your
  lanfix.log.


KNOWN ISSUES
------------
- With a VPN running, the game can still work out the VPN address as its own; lanfix
  moves its connections to the right adapter, but the game may advertise the VPN
  address in the lobby. This can show your own name twice. Rejoining the lobby, or
  disconnecting the VPN, clears it.
- Tested on Red Alert 3 (Steam, version 1.13) on Windows 11 only. C&C3 and Kane's Wrath
  use the same engine and network code and are expected to work, but are untested.
- If you play online through C&C:Online with a VPN on, lanfix sends the game's traffic
  over your normal connection instead of the VPN. Set Enabled=0 for online play if you
  need the VPN.
