const DRIVE_API = "https://www.googleapis.com/drive/v3";
const DRIVE_UPLOAD_API = "https://www.googleapis.com/upload/drive/v3";
const GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";
const FOLDER_MIME_TYPE = "application/vnd.google-apps.folder";
const TXT_MIME_TYPE = "text/plain; charset=utf-8";
const DEFAULT_QUOTA_BYTES = 1073741824;

export default {
  async fetch(request, env) {
    if (request.method === "OPTIONS") {
      return corsResponse(null, { status: 204 });
    }

    try {
      return await handleRequest(request, env);
    } catch (error) {
      console.error(error);
      return jsonResponse({
        ok: false,
        message: error.message || "Online library failed",
      }, 500);
    }
  },
};

async function handleRequest(request, env) {
  const url = new URL(request.url);
  const match = url.pathname.match(/^\/api\/devices\/([^/]+)\/books(?:\/([^/]+)(?:\/content)?)?(?:\/resolve-name)?$/);

  if (!match) {
    return jsonResponse({ ok: false, message: "Not found" }, 404);
  }

  const deviceId = decodeURIComponent(match[1]);
  if (!isValidDeviceId(deviceId)) {
    return jsonResponse({ ok: false, message: "Invalid device id" }, 400);
  }

  const token = await getAccessToken(env);
  const deviceFolderId = await ensureDeviceFolder(env, token, deviceId);
  const path = url.pathname;

  if (request.method === "GET" && path.endsWith("/books")) {
    return jsonResponse(await listLibrary(env, token, deviceId, deviceFolderId));
  }

  if (request.method === "POST" && path.endsWith("/resolve-name")) {
    const body = await request.json().catch(() => ({}));
    const name = validateTxtName(body.name);
    const size = Number(body.size || 0);
    const library = await listLibrary(env, token, deviceId, deviceFolderId);
    const uniqueName = uniqueBookName(name, library.books);
    return jsonResponse({
      ok: true,
      deviceId,
      name: uniqueName,
      quota: library.quota,
      canStore: size <= library.quota.free,
    });
  }

  if (request.method === "POST" && path.endsWith("/books")) {
    return uploadBook(request, env, token, deviceId, deviceFolderId);
  }

  const fileId = match[2] ? decodeURIComponent(match[2]) : "";

  if (request.method === "GET" && path.endsWith("/content") && fileId) {
    return downloadBook(env, token, deviceFolderId, fileId);
  }

  if (request.method === "DELETE" && fileId) {
    await assertFileInDeviceFolder(env, token, deviceFolderId, fileId);
    await driveFetch(env, token, `/files/${encodeURIComponent(fileId)}`, {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ trashed: true }),
    });
    return jsonResponse(await listLibrary(env, token, deviceId, deviceFolderId));
  }

  return jsonResponse({ ok: false, message: "Unsupported method" }, 405);
}

function isValidDeviceId(value) {
  return /^acat-[0-9a-fA-F]{32}$/.test(value);
}

function validateTxtName(name) {
  const value = String(name || "").trim();
  if (!value || value.length > 96 || !value.toLowerCase().endsWith(".txt")) {
    throw new Error("Use a shorter .txt filename.");
  }
  if (/[\\/:*?"<>|\x00-\x1f\x7f]/.test(value)) {
    throw new Error("Use a filename without path or system characters.");
  }
  return value;
}

function uniqueBookName(name, books) {
  const existing = new Set(books.map((book) => book.name.toLowerCase()));
  if (!existing.has(name.toLowerCase())) {
    return name;
  }

  const dot = name.toLowerCase().lastIndexOf(".txt");
  const base = name.slice(0, dot);
  const extension = name.slice(dot);
  let index = 1;

  while (true) {
    const candidate = `${base} (${index})${extension}`;
    if (!existing.has(candidate.toLowerCase())) {
      return candidate;
    }
    index += 1;
  }
}

async function listLibrary(env, token, deviceId, deviceFolderId) {
  const books = await listBooks(env, token, deviceFolderId);
  const used = books.reduce((sum, book) => sum + book.size, 0);
  const total = quotaBytes(env);

  return {
    ok: true,
    deviceId,
    quota: {
      total,
      used,
      free: Math.max(total - used, 0),
    },
    books,
  };
}

async function listBooks(env, token, folderId) {
  const q = `'${escapeDriveQuery(folderId)}' in parents and trashed = false and mimeType != '${FOLDER_MIME_TYPE}'`;
  const params = new URLSearchParams({
    q,
    fields: "files(id,name,size,mimeType,createdTime,modifiedTime)",
    orderBy: "name_natural",
    pageSize: "1000",
  });
  const result = await driveFetch(env, token, `/files?${params.toString()}`);
  const data = await result.json();

  return (data.files || []).map((file) => ({
    id: file.id,
    name: file.name,
    size: Number(file.size || 0),
    mimeType: file.mimeType || "text/plain",
    createdTime: file.createdTime || "",
    modifiedTime: file.modifiedTime || "",
  }));
}

async function uploadBook(request, env, token, deviceId, deviceFolderId) {
  const form = await request.formData();
  const file = form.get("file");
  const requestedName = validateTxtName(form.get("name") || file?.name);

  if (!file || typeof file.arrayBuffer !== "function") {
    throw new Error("Missing upload file.");
  }

  const bytes = new Uint8Array(await file.arrayBuffer());
  if (bytes.byteLength === 0) {
    throw new Error("Empty files are not supported.");
  }

  const current = await listLibrary(env, token, deviceId, deviceFolderId);
  if (bytes.byteLength > current.quota.free) {
    return jsonResponse({
      ok: false,
      message: "Online library quota is full.",
      quota: current.quota,
    }, 413);
  }

  const finalName = uniqueBookName(requestedName, current.books);
  const uploaded = await uploadTextFile(env, token, deviceFolderId, finalName, bytes);
  const next = await listLibrary(env, token, deviceId, deviceFolderId);

  return jsonResponse({
    ok: true,
    deviceId,
    book: {
      id: uploaded.id,
      name: uploaded.name || finalName,
      size: Number(uploaded.size || bytes.byteLength),
      mimeType: uploaded.mimeType || "text/plain",
      createdTime: uploaded.createdTime || "",
      modifiedTime: uploaded.modifiedTime || "",
    },
    quota: next.quota,
    books: next.books,
  });
}

async function uploadTextFile(env, token, folderId, name, bytes) {
  const boundary = `acat-${crypto.randomUUID()}`;
  const metadata = {
    name,
    parents: [folderId],
    mimeType: "text/plain",
  };

  const body = new Blob([
    `--${boundary}\r\n`,
    "Content-Type: application/json; charset=UTF-8\r\n\r\n",
    JSON.stringify(metadata),
    `\r\n--${boundary}\r\n`,
    `Content-Type: ${TXT_MIME_TYPE}\r\n\r\n`,
    bytes,
    `\r\n--${boundary}--\r\n`,
  ], { type: `multipart/related; boundary=${boundary}` });

  const response = await fetch(`${DRIVE_UPLOAD_API}/files?uploadType=multipart&fields=id,name,size,mimeType,createdTime,modifiedTime`, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": `multipart/related; boundary=${boundary}`,
    },
    body,
  });

  if (!response.ok) {
    throw new Error(await driveErrorMessage(response));
  }

  return response.json();
}

async function downloadBook(env, token, deviceFolderId, fileId) {
  const file = await assertFileInDeviceFolder(env, token, deviceFolderId, fileId);
  const response = await driveFetch(env, token, `/files/${encodeURIComponent(fileId)}?alt=media`);
  const headers = new Headers({
    "Content-Type": TXT_MIME_TYPE,
    "Content-Disposition": `attachment; filename="${file.name.replace(/"/g, "")}"`,
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Expose-Headers": "Content-Disposition",
  });
  return new Response(response.body, { status: 200, headers });
}

async function ensureDeviceFolder(env, token, deviceId) {
  const rootId = env.GOOGLE_DRIVE_ROOT_FOLDER_ID;
  if (!rootId) {
    throw new Error("Worker is missing GOOGLE_DRIVE_ROOT_FOLDER_ID.");
  }

  const existing = await findFolder(env, token, rootId, deviceId);
  if (existing) {
    return existing.id;
  }

  const response = await driveFetch(env, token, "/files?fields=id,name", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      name: deviceId,
      mimeType: FOLDER_MIME_TYPE,
      parents: [rootId],
    }),
  });
  const folder = await response.json();
  return folder.id;
}

async function findFolder(env, token, parentId, name) {
  const q = [
    `'${escapeDriveQuery(parentId)}' in parents`,
    "trashed = false",
    `mimeType = '${FOLDER_MIME_TYPE}'`,
    `name = '${escapeDriveQuery(name)}'`,
  ].join(" and ");
  const params = new URLSearchParams({
    q,
    fields: "files(id,name)",
    pageSize: "1",
  });
  const response = await driveFetch(env, token, `/files?${params.toString()}`);
  const data = await response.json();
  return data.files?.[0] || null;
}

async function assertFileInDeviceFolder(env, token, folderId, fileId) {
  const response = await driveFetch(env, token, `/files/${encodeURIComponent(fileId)}?fields=id,name,size,parents,trashed`);
  const file = await response.json();
  if (file.trashed || !Array.isArray(file.parents) || !file.parents.includes(folderId)) {
    throw new Error("Book is not in this device library.");
  }
  return file;
}

async function getAccessToken(env) {
  const required = ["GOOGLE_CLIENT_ID", "GOOGLE_CLIENT_SECRET", "GOOGLE_REFRESH_TOKEN"];
  for (const key of required) {
    if (!env[key]) {
      throw new Error(`Worker is missing ${key}.`);
    }
  }

  const body = new URLSearchParams({
    client_id: env.GOOGLE_CLIENT_ID,
    client_secret: env.GOOGLE_CLIENT_SECRET,
    refresh_token: env.GOOGLE_REFRESH_TOKEN,
    grant_type: "refresh_token",
  });

  const response = await fetch(GOOGLE_TOKEN_URL, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body,
  });

  if (!response.ok) {
    throw new Error(await driveErrorMessage(response));
  }

  const data = await response.json();
  return data.access_token;
}

async function driveFetch(env, token, path, init = {}) {
  const response = await fetch(`${DRIVE_API}${path}`, {
    ...init,
    headers: {
      Authorization: `Bearer ${token}`,
      ...(init.headers || {}),
    },
  });

  if (!response.ok) {
    throw new Error(await driveErrorMessage(response));
  }

  return response;
}

async function driveErrorMessage(response) {
  const text = await response.text();
  try {
    const parsed = JSON.parse(text);
    return parsed.error?.message || parsed.error_description || text;
  } catch (_) {
    return text || `Request failed with ${response.status}`;
  }
}

function escapeDriveQuery(value) {
  return String(value).replace(/\\/g, "\\\\").replace(/'/g, "\\'");
}

function quotaBytes(env) {
  const configured = Number(env.ONLINE_LIBRARY_QUOTA_BYTES);
  return Number.isFinite(configured) && configured > 0 ? configured : DEFAULT_QUOTA_BYTES;
}

function jsonResponse(body, status = 200) {
  return corsResponse(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function corsResponse(body, init = {}) {
  const headers = new Headers(init.headers || {});
  headers.set("Access-Control-Allow-Origin", "*");
  headers.set("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
  headers.set("Access-Control-Allow-Headers", "Content-Type");
  return new Response(body, { ...init, headers });
}
