This is a SteamVR driver that translates headset-space dashboard and laser pointer transforms between a Steam Frame's standing space and the PC's, so the streamed SteamVR dashboard stays lined up with the Frame's menu when the playspace is moved by OVR Advanced Settings' space drag.

Tested on SteamVR 2.18.4 (steam build 25998080). Unlikely to work on newer builds without updating the offsets.

Install with: `"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" adddriver "path-to-driver-folder\framedragfix"`
Uninstall with: `"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe" removedriver "path-to-driver-folder\framedragfix"`

No support provided, this was made for myself, and will be updated at my own pace. You're on your own.