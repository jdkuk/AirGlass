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
