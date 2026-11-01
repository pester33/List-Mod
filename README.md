# GD Gaming List (Geode mod)

Official mod for the GD Gaming List (gdgl.x10.mx): browse the list inside Geometry Dash 2.2081 (Geode 5.11.2).

## Download
Every push builds the mod on GitHub Actions and publishes it here:
https://github.com/pester33/List-Mod/releases/download/latest-build/pester.gdgl-list.geode

Put the `.geode` file in `Geometry Dash/geode/mods` and restart GD.

## How it works
The list site embeds the full level list as JSON on its Roulette page
(`<script id="roulette-data">`). The mod downloads that page, reads the JSON and shows it in a popup.
If the site changes that part of the page, the mod shows "Couldn't read the list".
