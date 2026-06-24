# AdventureCatto Online Library Worker

Cloudflare Worker API that stores per-device online libraries in your Google Drive.

## What You Need

1. A Cloudflare account with Workers enabled.
2. A Google Cloud project with the Google Drive API enabled.
3. OAuth credentials for a web/server app.
4. A Google OAuth refresh token for your Google account with Drive file access.
5. A Google Drive folder that will contain all device libraries.

## Drive Layout

```text
AdventureCattoLibraries/
  acat-0123456789abcdef0123456789abcdef/
    Book One.txt
    Book One (1).txt
```

The folder name is the `deviceId` returned by the board over USB.

## Worker Secrets

Set these in Cloudflare before deploying:

```powershell
npx wrangler secret put GOOGLE_CLIENT_ID
npx wrangler secret put GOOGLE_CLIENT_SECRET
npx wrangler secret put GOOGLE_REFRESH_TOKEN
npx wrangler secret put GOOGLE_DRIVE_ROOT_FOLDER_ID
```

With the local Windows wrapper, the same commands are:

```powershell
.\wrangler-local.cmd secret put GOOGLE_CLIENT_ID
.\wrangler-local.cmd secret put GOOGLE_CLIENT_SECRET
.\wrangler-local.cmd secret put GOOGLE_REFRESH_TOKEN
.\wrangler-local.cmd secret put GOOGLE_DRIVE_ROOT_FOLDER_ID
```

`ONLINE_LIBRARY_QUOTA_BYTES` is configured in `wrangler.toml` and defaults to 1 GB per device.

## Deploy

```powershell
cd online-library-worker
npm install
npm run deploy
```

If `npm` is not installed on this Windows shell, Codex can install the dependencies with its bundled Node runtime. After that, use the local wrapper:

```powershell
cd online-library-worker
.\wrangler-local.cmd login
.\wrangler-local.cmd deploy
```

After deploy, copy the Worker URL into the website config:

```js
window.ADVENTURE_CATTO_ONLINE_LIBRARY_API_BASE = "https://your-worker.your-subdomain.workers.dev";
```

Or set it in the browser console while testing:

```js
localStorage.setItem("adventureCattoApiBase", "https://your-worker.your-subdomain.workers.dev");
location.reload();
```

## API

- `GET /api/devices/:deviceId/books`
- `POST /api/devices/:deviceId/books/resolve-name`
- `POST /api/devices/:deviceId/books`
- `GET /api/devices/:deviceId/books/:fileId/content`
- `DELETE /api/devices/:deviceId/books/:fileId`

There is intentionally no user login or API auth. Anyone with the Worker URL and a valid `deviceId` can edit that device's online library.
