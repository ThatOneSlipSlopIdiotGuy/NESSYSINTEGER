# Making a release

This page is for whoever publishes NESSYSINTEGER builds. `package.ps1` builds the emulator, zips it, and writes a Scoop manifest with the zip's SHA-256 hash.

## 1. Try it on your own PC first

```powershell
scoop install gcc     # once, if you don't have a compiler yet
.\package.ps1         # builds dist\NESSYSINTEGER-0.7.1-win64.zip and dist\nessysinteger.json,
                      # then serves the zip on http://localhost:8765 until you press Ctrl+C
```

Then, in a second terminal:

```powershell
scoop install .\dist\nessysinteger.json
```

## 2. Publish a release on GitHub

1. Build the release with your repository's name:

   ```powershell
   .\package.ps1 -Version 0.7.1 -Repo ThatOneSlipSlopIdiotGuy/NESSYSINTEGER
   ```

   If the repository has a different name, use that instead.
2. On GitHub, create a release tagged `v0.7.1` and attach `dist\NESSYSINTEGER-0.7.1-win64.zip`.

The zip contains `NESSYSINTEGER.exe`, `nessys_cli.exe`, `README.md` and the self-test ROMs.

## 3. Make it installable with Scoop (optional)

1. Create a bucket repository from the template at `github.com/ScoopInstaller/BucketTemplate`.
2. Copy `dist\nessysinteger.json` into its `bucket\` folder and push.
3. Anyone can then install NESSYSINTEGER with:

   ```powershell
   scoop bucket add nessys https://github.com/ThatOneSlipSlopIdiotGuy/YOUR-BUCKET-REPO
   scoop install nessys/nessysinteger
   ```

## Version numbers

The version comes from the `-Version` argument; the default is set at the top of `package.ps1`. When you release a new version, also update the version line near the top of `README.md`.

## Good to know

- If PowerShell refuses to run `package.ps1` because it's "not digitally signed", the files still carry Windows' downloaded-from-the-internet mark. Run `Get-ChildItem -Recurse | Unblock-File` once in this folder.
- Scoop caches downloads. If you rebuild without changing the version, run `scoop cache rm nessysinteger` before reinstalling, or bump `-Version`.
- Scoop keeps `nessysinteger.ini` across updates. That file holds the settings, key bindings, the console's fingerprint and the connector's condition. Saves, states, cheats and screenshots live next to the ROMs, so Scoop never touches them.
- `scoop\nessysinteger.json` declares `"license": "MIT"`. Make it match the license you choose for the repository.
