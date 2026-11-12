# GD Gaming List (Geode mod)

Official mod for the GD Gaming List (gdgl.x10.mx): browse the list inside Geometry Dash 2.2091 (Geode 6.0.0-alpha.1).

## Download
Every push builds the mod on GitHub Actions and publishes it here:
https://github.com/pester33/List-Mod/releases/download/latest-build-2.2091/pester.gdgl-list.geode

(The old `latest-build` release is the last version for GD 2.2081.)

Put the `.geode` file in `Geometry Dash/geode/mods` and restart GD.

## How it works
The list site embeds the full level list as JSON on its Roulette page
(`<script id="roulette-data">`). The mod downloads that page, reads the JSON and shows it in a popup.
If the site can't be loaded or reads wrong, the mod falls back to `data/levels.json` in this repo, which a GitHub Action refreshes from the site every 30 minutes.
