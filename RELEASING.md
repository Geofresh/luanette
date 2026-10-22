# Releasing

1. Bump the version in `cli/package.json` to the new one (`0.5.0`), and `kApiVersion` in
   `engine/cart.hpp` if the API changed. Commit and push, and wait for CI to pass.
2. Tag it and push the tag:

   ```bash
   git tag v0.5.0 && git push origin v0.5.0
   ```

   `.github/workflows/release.yml` then:
   - builds the firmware and makes the GitHub release with its four files
   - rebuilds the playground, so its Install button offers the new firmware
   - checks that `cli/package.json` says the same version as the tag, packs the npm package and
     attaches `luanette-0.5.0.tgz` to the release
3. Publish the npm package from that file. It needs the npm account's two-factor authentication,
   so a person does it:

   ```bash
   gh release download v0.5.0 --repo Geofresh/luanette --pattern 'luanette-*.tgz'
   npm publish luanette-0.5.0.tgz --auth-type=web
   ```

   Open the link it prints and approve. npm stages the version first: for about 20 minutes the
   package may show only a `0.0.0-stage` placeholder, and publishing again in that time fails with
   "previously staged". Wait, then check:

   ```bash
   npm view luanette version
   npx -y --prefer-online luanette@0.5.0 check examples/dice.lua
   ```
4. Check the live playground's `firmware/manifest.json` says the new version.
