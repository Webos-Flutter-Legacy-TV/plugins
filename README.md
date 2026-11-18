# Flutter webOS Legacy: plugins

LG's webOS plugins for Flutter, with fixes for **older webOS TV releases**.
Part of [Webos-Flutter-Legacy-TV](https://github.com/Webos-Flutter-Legacy-TV);
use it together with [flutter-webos](https://github.com/Webos-Flutter-Legacy-TV/flutter-webos)
and its `--legacy` build. **Website:** https://webos-flutter-legacy-tv.github.io

| Plugin | Changes in this repository |
| --- | --- |
| `video_player` | Loop position, position after Home, volume error at startup; moved or resized video stays visible |
| `video_player_drm` | Stops position polling once the player is gone |
| `audioplayers_soloud` | Resume after completion in release mode; `seek()` never hangs |
| `flutter_secure_storage` | Works without a system key service (webOS 6) by encrypting in the app sandbox |
| `webos_service_bridge` | Subscription cancel errors |

Use a plugin from here with a git dependency:

```yaml
dependencies:
  video_player_webos:
    git:
      url: https://github.com/Webos-Flutter-Legacy-TV/plugins.git
      ref: main
      path: packages/video_player
```

The other plugins are LG's, unchanged. Which plugins work on which webOS release
is on the [compatibility page](https://webos-flutter-legacy-tv.github.io/compatibility.html).

### Relationship to LG's plugins

This repository follows [lg-flutter-webos/plugins](https://github.com/lg-flutter-webos/plugins)
and regularly merges its changes. Each file keeps LG's licence header; some
carry LG terms other than BSD-3-Clause. Community project, not affiliated
with or endorsed by LG Electronics.

---

# Upstream documentation (LG's plugins)

# Plugin List 

To view the list of currently available plugins, please visit the **Plugin List Page**.
- [Plugin List Page](doc/plugin-list.md)
# Plugin Example Build Guide (Flutter-webOS)

## Prerequisites

Before proceeding, ensure your **Flutter-webOS** environment is correctly set up.

If your Flutter-webOS environment is not yet configured,
refer to the guide : [Flutter-webOS SDK Repository](https://github.com/lg-flutter-webos/flutter-webos)


To verify your setup:

- Verify installation status:

```bash
  flutter-webos doctor
  flutter-webos precache -f
```


## Flutter-webOS Environment Already Set Up

Proceed with the build steps below.
refer to the guide: [Plugin Example Build Guide](doc/plugin-guide.md)

> Case 1: Build an Existing Flutter-webOS Plugin Example

> Case 2: Add and Use a Plugin via Dependency

> Case 3: Create a New Flutter-webOS Plugin

## License
This project is licensed under the [LICENSE](LICENSES/) file.