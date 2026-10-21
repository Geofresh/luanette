# ESP Web Tools 10.4.0

The playground's Install button. These are the files of `dist/web/` from the npm package
`esp-web-tools@10.4.0` (https://github.com/esphome/esp-web-tools), unmodified. The tarball's
sha512 matched the registry's (`3pwkeFFm5Fj7UQo8SJNYK5RXrtNCpq6X9QoI6bMT4GBZWgrJqjn0YvM9ihG74BtMoSFYXfmDtkehuxe50PTMPQ==`).

They're kept here rather than loaded from a CDN because the script can write to a board the page has been
allowed to use: everything that can do that comes from this repository.

To update: `npm pack esp-web-tools@VERSION`, check its sha512 against
`npm view esp-web-tools@VERSION dist.integrity`, replace these files with its `dist/web/`, and change
the version here, in `web/playground.js` and in `NOTICE`.

License: Apache 2.0 (`LICENSES/Apache-2.0.txt`); the libraries it bundles are in
`LICENSES/esp-web-tools-bundle.txt`.
