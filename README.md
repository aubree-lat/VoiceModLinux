# Voicemod on Linux

Use the real **Voicemod** voice changer and soundboard on Linux. Your voice comes out of a
virtual microphone called **Voicemod** that Discord, games, OBS and anything else can pick.

> **Unofficial.** Not made, endorsed or supported by Voicemod S.L. You need your own Voicemod
> account (free works; Pro features need Pro, exactly as on Windows). This project contains **no
> Voicemod files**: you download Voicemod's own installer yourself and it runs as-is. Read
> [the honest warnings](#honest-warnings) before you start.

---

## The quick way

Open a terminal (press the **Super** key, type `Terminal`, press Enter) and paste this one line:

```
curl -fsSL https://raw.githubusercontent.com/aubree-lat/VoiceModLinux/main/get.sh | bash
```

It downloads this project and walks you through the rest in plain words (see "step 3" below for
what it asks). You can [read the script first](get.sh); it's short. Prefer to do it by hand? Keep
reading.

## Start here (first time on Linux?)

These steps assume **Fedora Workstation**, which is the easiest to start with because it already
has what this needs. Other Linux systems work too (see the end).

### 0. Get Fedora (if you're still on Windows)
[hop](https://github.com/Arjun-Ingole/hop) installs Fedora next to Windows without a USB stick.
Pick "Fedora Workstation". AMD graphics work out of the box.

### 1. Get this folder
Unzip it into your **Home** folder, so it ends up as `~/VoiceModLinux`.

### 2. Open a terminal
Press the **Super** key (the Windows key), type `Terminal`, press Enter.

### 3. Run the installer
Copy and paste these two lines, pressing Enter after each:

```
cd ~/VoiceModLinux
bash install.sh
```

It explains each step in plain words. It will:

- ask your password once, to install a few programs from Fedora's own software sources;
- ask you to download **Voicemod's installer** from [voicemod.net](https://www.voicemod.net/)
  (it opens the page for you; sign in and download the Windows installer);
- install Voicemod (about 400 MB, several minutes: a few small windows may flash, that's normal);
- optionally let your account read the keyboard so **hotkeys** work (it asks first).

### 4. Start Voicemod
Press **Super**, type `Voicemod`, press Enter. The first start takes about a minute.

1. Click **Get Started** and sign in. Your browser opens for the login, then returns to Voicemod.
2. Open **Settings → Audio** and set **Input** to your real microphone.
   Choose the one named after your microphone. **Don't** pick "PulseAudio Input" or anything
   starting with "Monitor of": those are silent. You only do this once.
3. In Discord, a game or OBS, choose the microphone called **Voicemod**.

That's it. Pick a voice in Voicemod and talk.

> **Tip:** if Discord doesn't list "Voicemod", close and reopen Discord. Programs look for
> microphones when they start. Also turn off Discord's *noise suppression* and *echo cancellation*,
> or they will cancel the effect.

---

## Hotkeys

Voicemod's own hotkeys can't see your keyboard on Linux, so this project does it for you.
**Any hotkey you set inside Voicemod is copied automatically** within a few seconds. Nothing to do.

Hotkeys only work if your account was allowed to read the keyboard (the installer asks; you must
**log out and back in once** after saying yes). Check with:

```
vmwine hotkeys list
```

Extra hotkeys of your own:

```
vmwine hotkeys add F9 "voice-changer toggle"
vmwine hotkeys add alt+K "sound Airhorn"
vmwine hotkeys remove F9
vmwine hotkeys sync off      # stop copying Voicemod's hotkeys
```

Find sound and voice names with `vmwine ctl sound list` and `vmwine ctl voice list`.
A plain letter (like `K`) fires every time you type it in *any* program, just like on Windows;
function keys, the number pad or a combination like `alt+K` avoid that.

---

## Everyday commands

| What | Command |
|---|---|
| Start Voicemod | search "Voicemod" in the app menu, or `vmwine` |
| Stop it completely (and remove the virtual mic) | `vmwine stop` |
| Change voice from a keyboard shortcut or script | `vmwine ctl voice Banana` |
| Mute / unmute | `vmwine ctl mute toggle` |
| Remove everything | `bash uninstall.sh` |

Closing the Voicemod window sends it to the system tray if your desktop has one (Fedora's GNOME
doesn't by default, so closing quits it). The virtual mic only exists while Voicemod is running.

---

## If something goes wrong

| Problem | What to do |
|---|---|
| Nothing opens for a minute | Normal on first start. Wait up to 90 seconds. |
| Red **"Audio Down"** banner | It clears itself in about 10 seconds. If not, re-pick your mic in Settings → Audio. |
| People can't hear you | In Voicemod, is your real mic the **Input**? In Discord, is the mic **Voicemod**? |
| No "Voicemod" mic in Discord | Close and reopen Discord. |
| Hotkeys do nothing | Run `vmwine hotkeys list`. If it says a keyboard is "NOT readable", log out and back in. |
| Everything is stuck | `vmwine stop`, then start it again. |

Logs for bug reports: `~/.local/share/vmwine/prefix/drive_c/users/*/AppData/Local/VoicemodV3/logs/voicemod-desktop.log`

---

## Honest warnings

- **It may break when Voicemod updates.** This works by patching Voicemod's behaviour in memory,
  against specific versions (tested: **3.17.43 and 3.17.45**). A big Voicemod update can break it
  until this project is updated. Updates are blocked on purpose so it doesn't update itself into a
  broken state.
- **Terms of Use.** Running Voicemod this way is not something Voicemod supports, and it may go
  against [their Terms of Use](https://www.voicemod.net/terms-of-use/). Use it at your own risk and
  with your own account. This project doesn't bypass any login, licence or payment check.
- **Only tested on one setup so far:** the author's CachyOS machine (Wine 11.21, PipeWire, niri).
  Fedora is the recommended system but its install path is new, so please report anything odd.
- **Not everything in Voicemod works yet.** Push-to-talk and a few other hotkey actions aren't
  copied yet, and the "music through the mic" feature isn't included.
- On GNOME (Fedora's default) there is no tray icon unless you add the *AppIndicator* extension.

## Other Linux systems

`bash install.sh` also knows **Arch/CachyOS** and **Ubuntu/Linux Mint/Debian**. Ubuntu-family
systems ship an old Wine, so you must first add
[WineHQ's repository](https://wiki.winehq.org/Ubuntu) to get Wine 11 or newer; the installer tells
you if that's needed.

---

## How it works (short)

Voicemod is a Windows program whose engine needs a Windows audio driver. Here the engine runs
under **Wine**; a tiny stand-in driver and some in-memory patches (from
[vmwine](https://github.com/GladCubes/voicemod-for-linux)) get it started; PipeWire provides the
virtual microphone; and Voicemod's own interface (taken from *your* copy of `Voicemod.exe`) is
shown in a native window using **Electron**, an idea from
[voicemod-linux](https://github.com/AriCuteGirl/voicemod-linux). Two helper scripts keep the
audio wiring right and mirror your hotkeys.

Why not Wine's own window? Voicemod's built-in browser crashes under Wine's graphics layer on
current Voicemod versions, so the interface is shown outside Wine instead.

## Credits and licences

This project is MIT licensed (see `LICENSE`). It includes code from, and is built on ideas from:

- **vmwine** by GladCubes, MIT: the stand-in driver and in-memory patches (`src/`, `vmwine`).
  Licence kept in `third_party/vmwine-LICENSE`.
- **voicemod-linux** by AriCuteGirl, MIT: the Electron window and interface extractor (`ui/`,
  `scripts/extract_qrc.py`, `tools/voicemod-ctl`). Licence in
  `third_party/voicemod-linux-AriCuteGirl-LICENSE`.
- **linuxpad** by aubree, MIT: the PipeWire routing ideas behind `tools/vmaudio.py`.

"Voicemod" is a trademark of Voicemod S.L.
