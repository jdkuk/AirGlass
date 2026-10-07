# Releasing

Releases are built by GitHub Actions from a version tag. Nothing is uploaded from a developer machine.

## Versioning

[Semantic Versioning](https://semver.org): `MAJOR.MINOR.PATCH`. The tag is `v1.2.3`. Pre-releases use `v1.3.0-beta.1`
and are published as GitHub *pre-releases*.

## Checklist

1. Update [`CHANGELOG.md`](../CHANGELOG.md): move items from *Unreleased* to a new `## [1.2.3] - YYYY-MM-DD` section.
2. Bump the version in [`assets/AirGlass.rc`](../assets/AirGlass.rc) (`FILEVERSION`, `PRODUCTVERSION` and the strings).
3. Make sure `src/license.h` has the real Lemon Squeezy `kStoreId`, `kProductId` and `kBuyUrl`. `package.ps1` refuses
   to build otherwise.
4. Commit, then tag and push:
   ```powershell
   git tag -a v1.2.3 -m "AirGlass 1.2.3"
   git push origin v1.2.3
   ```
5. The [release workflow](../.github/workflows/release.yml) then:
   * fetches the pinned toolchain and FFmpeg, and builds the release (never `--pro`)
   * runs the self-tests
   * builds `AirGlass-Setup-1.2.3.exe` with Inno Setup and `AirGlass-1.2.3-source.zip`
   * writes `SHA256SUMS.txt`
   * publishes a GitHub Release with notes generated from the merged PRs (categories from
     [`.github/release.yml`](../.github/release.yml)), plus the changelog section
6. Check the release page, then announce it.

## Code signing

Unsigned installers trigger Windows SmartScreen. When a signing certificate is available (for example Azure Trusted
Signing), add a signing step before the upload in `release.yml`, using repository secrets. Never commit keys.

## Store settings (Lemon Squeezy)

| Setting | Value |
|---|---|
| Product | AirGlass Pro, single payment |
| License keys | on, activation limit 3, no expiry |
| `kStoreId` / `kProductId` | from the dashboard (Settings → Stores, and the product URL) |
| `kBuyUrl` | the product's checkout (share) link |
