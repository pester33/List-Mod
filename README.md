# GD Gaming List (Geode mod)

Official mod for the GD Gaming List (gdgl.x10.mx): browse the list inside Geometry Dash 2.2091 (Geode 6.0.0-alpha.1).

## Download
Every push builds the mod for both GD versions on GitHub Actions:

- **GD 2.2091** (Geode 6): https://github.com/pester33/List-Mod/releases/download/latest-build-2.2091/pester.gdgl-list.geode
- **GD 2.2081** (Geode 5.11.2): https://github.com/pester33/List-Mod/releases/download/latest-build/pester.gdgl-list.geode

Put the `.geode` file in `Geometry Dash/geode/mods` and restart GD. After that the mod updates itself from inside the game
(each GD version only gets updates built for it).

## How it works
The list site embeds the full level list as JSON on its Roulette page
(`<script id="roulette-data">`). The mod downloads that page, reads the JSON and shows it in a popup.
If the site can't be loaded or reads wrong, the mod falls back to `data/levels.json` in this repo, which a GitHub Action refreshes from the site every 30 minutes.
