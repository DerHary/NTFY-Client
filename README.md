# ntfy Windows Client

A native Windows client for self-hosted ntfy servers. The receiver starts in the background when you sign in and displays incoming messages as Windows notifications.

## Installation

1. Run [NtfyWindows-Setup-x64.exe](bin/NtfyWindows-Setup-x64.exe) on Windows 10 (version 2004 or later) or Windows 11. The installer sets up the client for your Windows account.
2. Open **ntfy Settings** (**ntfy – Einstellungen**) from the Start menu and add a subscription. For example, enter `https://ntfy.example.org` as the server URL and `alerts` as the topic.
3. For a private topic, enter an ntfy access token with read access to that topic. Subscriptions that use a token require HTTPS.
4. Click **Apply** (**Übernehmen**), then **Save** (**Speichern**). The client restarts the receiver with the saved subscriptions.

The installer includes everything the client needs beyond Windows itself. You can close the settings window after saving. The receiver reconnects automatically when a connection drops. If the server caches messages, the receiver can catch up on missed messages as long as the same receiver process is still running.

Subscriptions are stored in `%LOCALAPPDATA%\NtfyWindows\subscriptions.json`. Windows DPAPI encrypts access tokens for your Windows account. Uninstalling from **Installed apps** removes the client and its startup entry. The subscription file is kept for reinstallation. To remove your saved subscriptions, delete the file after uninstalling.

If messages appear in Notification Center without a banner, check the ntfy banner setting and **Do not disturb** under **Settings → System → Notifications**.

## Building from source

You need the Visual Studio 2022 x64 C++ Build Tools, a Windows SDK (tested with 10.0.26100.0), and Inno Setup 6.7 or later (tested with 6.7.4). Run the build from the repository root.

```powershell
.\src\build.cmd "C:\Path\to\Inno Setup 6\ISCC.exe"
```

The build writes intermediate files to `..\.build\ntfy-client` outside the repository and the finished installer to `bin`. The client uses C++17, Win32, WinHTTP, and C++/WinRT from the Windows SDK. The C++ runtime is linked statically.

[ntfy HTTP API](https://docs.ntfy.sh/subscribe/api/) · [Walzen's ntfy-toaster technical reference](https://github.com/Walzen-Group/ntfy-toaster) · [MIT license](LICENSE)
