# Luminous Injector
A simple DLL injector that injects a DLL into another process using LoadLibrary.


Edit the config.json file to specify the target application name (processName) and the path of the DLL to be injected (dllPath).
Run the program and initiate the injection process.
<br><br>

`config.json example`

```json
{
  "processName": "DEVOUR.exe",
  "dllPath": "D:/SteamLibrary/steamapps/common/Devour/",
  "dllName": "DevourX.dll"
}
```
