# Port home screen

The home screen every port uses: the same layout and behaviour, dressed in each game's own look. This port's implementation is `android/app/src/main/java/io/github/belmantegu/raymanrecomp/LauncherActivity.java`.

<img src="media/android-s23-home.jpg" alt="Home screen on a Galaxy S23" width="780">

<img src="media/android-s23-home-options.jpg" alt="Options open on the home screen" width="780">

## Layout

All sizes are in a unit `u` = 1% of the screen width, bounded by the height so the layout also fits 4:3 screens and tablets.

| Area | Position | Content |
|---|---|---|
| Presented by | top left | "BELMANTEGU PRESENTS", small caps |
| Logo | top left, 33u wide | the game's logo (transparent PNG) |
| Menu | left, a panel 24u wide | **Play**, **Options**, **Game files**, **Quit** |
| Status | bottom left, a panel | game files ready (platform, file count, size) or missing; import progress |
| Version | bottom right | the app's version |
| Controls | top right | A Select, B Back |
| Options | right, a panel 36u wide, opened by Options | choosers: ◀ value ▶ |

The selected menu item is highlighted and gets ◀ ▶ arrows.

## Behaviour

- **Play** starts the game when the files are in place; otherwise it says to import them.
- **Game files** imports the player's own copy as one game pack (.zip, `tools/make_game_pack.py`), with a progress bar and a free-space check, then starts the game.
- **Options** holds the render resolution (50–100%), the graphics path (native renderer or emulation), the touch controls and their opacity, importing from a folder, and importing saves. Values change with ◀ ▶ or a tap and are saved right away.
- **Quit** closes the app.
- Touch, controllers (D-pad, stick, A, B) and keyboards (arrows, Enter, Esc) all drive it.

## Look: taken from the game

Each port takes its palette and panel style from the game's own menus, and its background from the game itself.

For Rayman Origins (the game's options menu):

| Role | Color |
|---|---|
| Panels: dark red wooden planks, with a lighter rim | `#7d1710` / `#a3301c` |
| Buttons | `#e3642b` |
| Selected item and choosers | `#f6b02c` |
| Text on panels | `#fdebc8` |

The panels and buttons have rough, uneven outlines, like the game's planks. Fonts: Titan One for labels, Barlow Semi Condensed for small text (SIL OFL, bundled).

**Background:** a looping video from the game (here, the intro's great-tree shot, played forward then backward so the loop has no cut). The ground is dark, with a light gradient: a shaft of light from above. The menu side is darkened so the panels stay readable.

## Art comes from the player's copy

The logo and the background video are the game's art, so they never ship in the repository or in a published build. A personal build packs them from `private/launcher/` (`android/build_apk.sh`):

| File | Used for | Fallback |
|---|---|---|
| `logo.png` | the logo | the title in the display font |
| `bg.mp4` | the looping background | `background.jpg`, then a dark gradient |
