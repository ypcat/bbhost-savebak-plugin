# bbhost-savebak-plugin

A plugin for [bbhost](https://github.com/droogie/bbhost) on Linux. At the
title screen, it backs up or restores the game save with the Xbox controller.
In the game, it exits to the title screen.

## Use

1. Go to the title screen.
2. Hold LT + RT + View + Menu. A banner shows `LT=load RT=save`.
3. Release all buttons.
4. Press one button:
   - RT copies `saves/SPRJ0005/*` to `saves/bak/`. It creates `saves/bak/`
     if it does not exist.
   - LT copies `saves/bak/*` to `saves/SPRJ0005/`. Then select Continue to
     load the restored save. If there is no backup, the banner shows
     `No backup`.
   - B, the d-pad, or any other button cancels.
5. If you press nothing for 10 seconds, the choice closes.

In the game, the combination exits to the title at once, with no question.
It calls the YES action of System > Exit Game (`0x200d050` in the 1.09
eboot, the action that `sub_200c9f0` builds). To use it again, release all
four buttons.

The folders are in the bbhost data folder (`~/.local/share/bbhost/data`).
Each file is written to a `.tmp` file first, then renamed.

## Build and install

```
make
make install
```

`make install` copies `savebak.so` to `~/.config/bbhost/plugins`. Start
bbhost again. The log shows `plugin: savebak 1.1.0 loaded` and
`savebak: reading /dev/input/event<N>`.

The plugin is not signed. If `plugins.signed_only` is set in `bbhost.toml`,
bbhost does not load it.

## Limits

- The plugin reads the controller from `/dev/input`, so bbhost also gets the
  buttons. B is the safe button to cancel at the title screen.
- It reads up to 4 controllers, with a Guide button each.

## Files

- `savebak.c`: the plugin.
- `bbhost_plugin.h`: the bbhost plugin API header, copied from
  bbhost-linux-v0.2.18.

## License

GNU GPL, version 3 or later. See `LICENSE`. `bbhost_plugin.h` is part of
bbhost, which has the same license.
