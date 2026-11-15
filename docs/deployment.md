# Deploying AirGlass

## Naming each PC

Users rename their PC from the tray icon: right-click → **Rename this PC…**. The new name is saved as `name=` under
`[receiver]` in `%LOCALAPPDATA%\AirGlass\config.ini`, and iPhones, iPads and Macs see it straight away. No restart is needed.

## A machine-wide name (IT departments)

To set the name centrally (one per room, say), put it in `%ProgramData%\AirGlass\airglass.ini`:

```ini
[receiver]
name=Meeting Room 2
```

* This name wins over the user's own setting, for every user on the PC. The tray menu then shows
  *Name set by your organisation* instead of *Rename this PC…*.
* AirGlass reads it at start-up, so restart AirGlass (or sign out and back in) after changing it.
* The same rules apply as for any receiver name: up to 40 bytes, and dots become spaces. If another AirPlay device on
  the network already uses the name, AirGlass adds " PC" for that run.
* Deploy the file with Group Policy Preferences, Intune or any software distribution tool. Users only need read access.
  Save it as UTF-8 or UTF-16 with a BOM if the name has characters outside plain ASCII.
* Delete the file, or the `name=` line, to let users choose again.

## Ask before new devices connect

With this on, a device AirGlass hasn't seen before can't just start mirroring or playing audio: the PC asks
*Allow “Jonathan's iPhone” to use AirGlass?* with **Allow once**, **Always allow** and **Block**. Nobody answering
within 30 seconds, closing the prompt, or **Block** turns the device away, and it shows an error. Devices that are
always allowed or blocked are not asked again.

Users turn it on from the tray icon: **Ask before new devices connect** (AirGlass Pro). To turn it on for every user
of a PC, add this to `%ProgramData%\AirGlass\airglass.ini`:

```ini
[security]
askNewDevices=1
```

* The machine-wide setting works in every edition, and users can't turn it off: the tray menu shows it ticked and
  greyed, *(set by your organisation)*. Restart AirGlass after changing it.
* Each user's answers are kept in their `%LOCALAPPDATA%\AirGlass\config.ini`, under `[devices]`, one line per
  device: the sender's AirPlay device id, `allow` or `block`, and the name it gave, for example
  `AA:BB:CC:DD:EE:FF=allow Jonathan's iPhone`. Tray → **Forget allowed and blocked devices** clears the list. An
  iPhone with *Private Wi-Fi Address* on may have a different id on another network, so it is asked once more there.
* A user who turns it on during the Pro trial keeps it on after the trial, and can still turn it off.
