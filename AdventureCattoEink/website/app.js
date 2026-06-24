const BAUD_RATE = 115200;
const CHUNK_SIZE = 1024;
const PREFIX = "ACAT ";
const OPEN_TIMEOUT_MS = 5000;
const HANDSHAKE_TIMEOUT_MS = 7000;
const WRITE_TIMEOUT_MS = 2500;

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const SUPPORTED_EXTENSIONS = new Set(["txt", "epub", "pdf"]);
const PDF_TEXT_MIN_CHARS = 80;
const ONLINE_LIBRARY_API_BASE = (
  localStorage.getItem("adventureCattoApiBase")
  || window.ADVENTURE_CATTO_ONLINE_LIBRARY_API_BASE
  || ""
).replace(/\/+$/, "");

if (window.pdfjsLib) {
  window.pdfjsLib.GlobalWorkerOptions.workerSrc = "./vendor/pdf.worker.min.js";
}

let port = null;
let reader = null;
let writer = null;
let lineBuffer = "";
let busy = false;
let connecting = false;
let knownPorts = [];
let heartbeatTimer = 0;

const pendingResponses = [];
const state = {
  deviceId: "",
  storage: null,
  books: [],
  onlineStorage: null,
  onlineBooks: [],
  onlineAvailable: false,
};

const els = {
  browserNotice: document.getElementById("browserNotice"),
  connectButton: document.getElementById("connectButton"),
  portSelect: document.getElementById("portSelect"),
  connectionState: document.getElementById("connectionState"),
  statusText: document.getElementById("statusText"),
  storageFill: document.getElementById("storageFill"),
  usedNumber: document.getElementById("usedNumber"),
  usedUnit: document.getElementById("usedUnit"),
  totalBytes: document.getElementById("totalBytes"),
  freeBytes: document.getElementById("freeBytes"),
  storageDetail: document.getElementById("storageDetail"),
  bookRows: document.getElementById("bookRows"),
  bookCount: document.getElementById("bookCount"),
  onlineLibrarySection: document.getElementById("onlineLibrarySection"),
  onlineBookRows: document.getElementById("onlineBookRows"),
  onlineBookCount: document.getElementById("onlineBookCount"),
  onlineStorageText: document.getElementById("onlineStorageText"),
  onlineAddButton: document.getElementById("onlineAddButton"),
  onlineFileInput: document.getElementById("onlineFileInput"),
  fileInput: document.getElementById("fileInput"),
  dropZone: document.getElementById("dropZone"),
  uploadProgress: document.getElementById("uploadProgress"),
  uploadFill: document.getElementById("uploadFill"),
  uploadText: document.getElementById("uploadText"),
};

if (!("serial" in navigator)) {
  els.browserNotice.hidden = false;
  els.connectButton.disabled = true;
  els.portSelect.disabled = true;
} else {
  populateKnownPorts();
  navigator.serial.addEventListener("connect", (event) => {
    populateKnownPorts();
    if (!port && !connecting) {
      setStatus("Device detected. Press Connect.", "good");
    }
  });
  navigator.serial.addEventListener("disconnect", (event) => {
    if (event.target === port) {
      disconnectDevice(false, false);
      setStatus("Disconnected.");
    }
  });
}

els.connectButton.addEventListener("click", () => {
  if (port) {
    disconnectDevice();
  } else {
    connectDevice();
  }
});

window.addEventListener("beforeunload", () => {
  if (writer) {
    writer.write(encoder.encode("ACAT BYE\n")).catch(() => {});
  }
});

els.fileInput.addEventListener("change", () => {
  const file = els.fileInput.files?.[0];
  if (file) {
    uploadFile(file);
  }
});

els.onlineAddButton.addEventListener("click", () => {
  els.onlineFileInput.click();
});

els.onlineFileInput.addEventListener("change", () => {
  const file = els.onlineFileInput.files?.[0];
  if (file) {
    uploadFileToOnlineLibrary(file);
  }
});

["dragenter", "dragover"].forEach((eventName) => {
  els.dropZone.addEventListener(eventName, (event) => {
    event.preventDefault();
    if (!busy && port) {
      els.dropZone.classList.add("dragging");
    }
  });
});

["dragleave", "drop"].forEach((eventName) => {
  els.dropZone.addEventListener(eventName, (event) => {
    event.preventDefault();
    els.dropZone.classList.remove("dragging");
  });
});

els.dropZone.addEventListener("drop", (event) => {
  const file = event.dataTransfer.files?.[0];
  if (file) {
    uploadFile(file);
  }
});

async function populateKnownPorts() {
  knownPorts = await navigator.serial.getPorts();
  renderPortOptions();
}

function renderPortOptions() {
  els.portSelect.replaceChildren();

  if (port) {
    const option = document.createElement("option");
    option.value = "connected";
    option.textContent = portLabel();
    els.portSelect.append(option);
    els.portSelect.value = "connected";
    return;
  }

  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = "Select device";
  els.portSelect.append(placeholder);

  knownPorts.forEach((knownPort, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    option.textContent = portLabel();
    els.portSelect.append(option);
  });

  if (knownPorts.length === 1) {
    els.portSelect.value = "0";
  }
}

function portLabel() {
  return "Tubelight's thingamajig";
}

function setStatus(message, tone = "") {
  els.statusText.textContent = message;
  els.statusText.className = `status-line ${tone}`.trim();
  els.statusText.hidden = !message;
}

function setBusy(nextBusy) {
  busy = nextBusy;
  els.connectButton.disabled = busy || !("serial" in navigator);
  els.portSelect.disabled = busy || Boolean(port) || !("serial" in navigator);
  els.fileInput.disabled = busy || !port;
  els.onlineFileInput.disabled = busy || !state.onlineAvailable;
  els.onlineAddButton.disabled = busy || !state.onlineAvailable;
  els.dropZone.classList.toggle("disabled", busy || !port);
  document.querySelectorAll(".row-action").forEach((button) => {
    button.disabled = busy || !port;
  });
}

function setConnected(isConnected, keepBusy = false) {
  els.connectionState.classList.toggle("connected", isConnected);
  els.connectionState.lastChild.textContent = isConnected ? " Connected" : " Disconnected";
  els.connectButton.textContent = isConnected ? "Disconnect" : "Connect";
  renderPortOptions();
  if (!keepBusy) {
    setBusy(false);
  }
}

async function connectDevice(preselectedPort = null) {
  if (connecting) {
    return;
  }

  connecting = true;

  try {
    setBusy(true);
    setStatus("Opening serial port...");

    const selectedIndex = Number.parseInt(els.portSelect.value, 10);
    port = preselectedPort
      || (Number.isInteger(selectedIndex) && knownPorts[selectedIndex]
      ? knownPorts[selectedIndex]
      : await navigator.serial.requestPort());

    await openPortWithTimeout(port, OPEN_TIMEOUT_MS);
    writer = port.writable.getWriter();
    readLoop();

    setConnected(true, true);
    startHeartbeat();
    setStatus("Checking device...");
    await wait(250);
    const hello = await sendCommand("ACAT HELLO", ["hello"], HANDSHAKE_TIMEOUT_MS);
    state.deviceId = hello.deviceId || "";
    await refreshLibrary(false);
    await refreshOnlineLibrary(false);
    setStatus("Connected.", "good");
  } catch (error) {
    await disconnectDevice(false, false);
    setStatus(error.message || "Connection failed.", "bad");
  } finally {
    connecting = false;
    setBusy(false);
  }
}

async function openPortWithTimeout(serialPort, timeoutMs) {
  let timeoutId = 0;
  const timeout = new Promise((_, reject) => {
    timeoutId = window.setTimeout(() => {
      reject(new Error("The serial port did not open. Unplug and reconnect the device, then try Connect again."));
    }, timeoutMs);
  });

  try {
    await Promise.race([
      serialPort.open({ baudRate: BAUD_RATE }),
      timeout,
    ]);
  } finally {
    window.clearTimeout(timeoutId);
  }
}

async function notifyDeviceBeforeDisconnect(activeWriter) {
  if (!activeWriter) {
    return;
  }

  await settleWithin(activeWriter.write(encoder.encode("ACAT BYE\n")), 300);
}

async function disconnectDevice(showStatus = true, notifyDevice = true) {
  const closingPort = port;
  const closingReader = reader;
  const closingWriter = writer;

  stopHeartbeat();
  port = null;
  reader = null;
  writer = null;
  lineBuffer = "";
  state.deviceId = "";
  resetOnlineLibrary();
  rejectPending("Disconnected.");
  setConnected(false);

  if (showStatus) {
    setStatus("Disconnected.");
  }

  if (notifyDevice) {
    await notifyDeviceBeforeDisconnect(closingWriter);
  }

  try {
    if (closingReader) {
      await settleWithin(closingReader.cancel(), 500);
    }
  } catch (_) {
  }

  try {
    if (closingWriter) {
      closingWriter.releaseLock();
    }
  } catch (_) {
  }

  try {
    if (closingPort) {
      await settleWithin(closingPort.close(), 1000);
    }
  } catch (_) {
  }

  populateKnownPorts().catch(() => {});
}

async function readLoop() {
  try {
    while (port?.readable) {
      const activePort = port;
      const activeReader = activePort.readable.getReader();
      reader = activeReader;
      try {
        while (true) {
          const { value, done } = await activeReader.read();
          if (done) {
            break;
          }
          if (value) {
            consumeSerialText(decoder.decode(value, { stream: true }));
          }
        }
      } finally {
        activeReader.releaseLock();
        if (reader === activeReader) {
          reader = null;
        }
      }
    }
  } catch (error) {
    if (port) {
      setStatus(error.message || "Serial connection closed.", "bad");
    }
  }
}

function consumeSerialText(text) {
  lineBuffer += text;
  let newlineAt = lineBuffer.indexOf("\n");

  while (newlineAt >= 0) {
    const line = lineBuffer.slice(0, newlineAt).trim();
    lineBuffer = lineBuffer.slice(newlineAt + 1);
    handleSerialLine(line);
    newlineAt = lineBuffer.indexOf("\n");
  }
}

function handleSerialLine(line) {
  if (!line.startsWith(PREFIX)) {
    return;
  }

  try {
    const message = JSON.parse(line.slice(PREFIX.length));
    if (message.storage) {
      updateStorage(message.storage);
    }
    if (Array.isArray(message.books)) {
      updateBooks(message.books);
    }
    resolvePending(message);
  } catch (_) {
    setStatus("Device response could not be read.", "bad");
  }
}

function createResponseWaiter(expectedTypes, timeoutMs) {
  const expected = new Set(expectedTypes);
  let waiter = null;
  const promise = new Promise((resolve, reject) => {
    waiter = {
      expected,
      resolve,
      reject,
      timer: window.setTimeout(() => {
        removePending(waiter);
        reject(new Error("Timed out waiting for the device."));
      }, timeoutMs),
    };
  });

  pendingResponses.push(waiter);

  return {
    promise,
    cancel() {
      removePending(waiter);
      window.clearTimeout(waiter.timer);
    },
  };
}

function resolvePending(message) {
  const index = pendingResponses.findIndex((waiter) => message.ok === false || waiter.expected.has(message.type));
  if (index < 0) {
    return;
  }

  const [waiter] = pendingResponses.splice(index, 1);
  window.clearTimeout(waiter.timer);
  waiter.resolve(message);
}

function removePending(waiter) {
  const index = pendingResponses.indexOf(waiter);
  if (index >= 0) {
    pendingResponses.splice(index, 1);
  }
}

function rejectPending(message) {
  while (pendingResponses.length > 0) {
    const waiter = pendingResponses.shift();
    window.clearTimeout(waiter.timer);
    waiter.reject(new Error(message));
  }
}

async function sendCommand(command, expectedTypes, timeoutMs = 8000) {
  if (!writer) {
    throw new Error("Connect the device first.");
  }

  const responseWaiter = createResponseWaiter(expectedTypes, timeoutMs);

  let response;
  try {
    await rejectAfter(
      writer.write(encoder.encode(command.endsWith("\n") ? command : `${command}\n`)),
      Math.min(timeoutMs, WRITE_TIMEOUT_MS),
      "Could not send data to the device. Try Connect again.",
    );
    response = await responseWaiter.promise;
  } catch (error) {
    responseWaiter.cancel();
    throw error;
  }

  if (!response.ok) {
    throw new Error(response.message || "Device rejected the request.");
  }

  return response;
}

async function refreshLibrary(showMessage = true) {
  try {
    if (showMessage) {
      setBusy(true);
      setStatus("Reading library...");
    }
    await sendCommand("ACAT LIST", ["list"], 8000);
    if (showMessage) {
      setStatus("Library refreshed.", "good");
    }
  } catch (error) {
    setStatus(error.message || "Could not refresh library.", "bad");
  } finally {
    if (showMessage) {
      setBusy(false);
    }
  }
}

async function deleteBook(book) {
  if (!window.confirm(`Delete "${book.name}" from the device?`)) {
    return;
  }

  try {
    setBusy(true);
    setStatus(`Deleting ${book.name}...`);
    await sendCommand(`ACAT DELETE ${textToBase64(book.name)}`, ["list"], 10000);
    setStatus("Title deleted.", "good");
  } catch (error) {
    setStatus(error.message || "Delete failed.", "bad");
  } finally {
    setBusy(false);
  }
}

async function uploadFile(file) {
  try {
    if (!port) {
      throw new Error("Connect the device first.");
    }

    let upload = await prepareUpload(file);
    let onlineSyncMessage = "";
    let shouldSyncOnline = true;

    if (canUseOnlineLibrary()) {
      try {
        const resolved = await resolveOnlineName(upload.name, upload.bytes.length);
        upload = {
          ...upload,
          name: resolved.name,
        };
        if (resolved.canStore === false) {
          shouldSyncOnline = false;
          onlineSyncMessage = " Online library is full; uploaded to device only.";
        }
      } catch (error) {
        shouldSyncOnline = false;
        onlineSyncMessage = ` Online server is down: ${error.message || "could not reserve name"}.`;
      }
    }

    await ensureStorageFresh();
    validateStorageForUpload(upload);

    await uploadPreparedToDevice(upload);

    if (canUseOnlineLibrary() && shouldSyncOnline) {
      try {
        await uploadPreparedToOnlineLibrary(upload, false);
      } catch (error) {
        onlineSyncMessage = ` Online server is down: ${error.message || "backup failed"}.`;
      }
    }

    setStatus(`Title uploaded.${onlineSyncMessage}`, onlineSyncMessage ? "bad" : "good");
  } catch (error) {
    setStatus(error.message || "Upload failed.", "bad");
  } finally {
    els.fileInput.value = "";
    window.setTimeout(() => {
      els.uploadProgress.hidden = true;
    }, 900);
    setBusy(false);
  }
}

async function uploadPreparedToDevice(upload) {
  let uploadStarted = false;

  try {
    setBusy(true);
    showUploadProgress(0, "Starting");

    await sendCommand(`ACAT BEGIN ${textToBase64(upload.name)} ${upload.bytes.length}`, ["begin"], 10000);
    uploadStarted = true;

    const bytes = upload.bytes;
    let offset = 0;

    while (offset < bytes.length) {
      const chunk = bytes.slice(offset, offset + CHUNK_SIZE);
      await sendCommand(`ACAT DATA ${bytesToBase64(chunk)}`, ["data"], 12000);
      offset += chunk.length;
      showUploadProgress(offset / bytes.length, `${Math.round((offset / bytes.length) * 100)}%`);
    }

    await sendCommand("ACAT END", ["list"], 15000);
    showUploadProgress(1, "Complete");
  } catch (error) {
    if (uploadStarted) {
      sendCommand("ACAT CANCEL", ["cancel"], 3000).catch(() => {});
    }
    throw error;
  }
}

function canUseOnlineLibrary() {
  return Boolean(ONLINE_LIBRARY_API_BASE && state.deviceId);
}

async function refreshOnlineLibrary(showMessage = true) {
  if (!state.deviceId) {
    resetOnlineLibrary();
    return;
  }

  els.onlineLibrarySection.hidden = false;

  if (!ONLINE_LIBRARY_API_BASE) {
    state.onlineAvailable = false;
    renderOnlineLibraryMessage("Online library is not configured.");
    setBusy(false);
    return;
  }

  try {
    if (showMessage) {
      setBusy(true);
      setStatus("Reading online library...");
    }
    const data = await onlineJson(`/api/devices/${encodeURIComponent(state.deviceId)}/books`);
    updateOnlineLibrary(data);
    if (showMessage) {
      setStatus("Online library refreshed.", "good");
    }
  } catch (error) {
    state.onlineAvailable = false;
    renderOnlineLibraryMessage(`Online server is down: ${error.message || "could not load library"}.`);
    if (showMessage) {
      setStatus("Online server is down.", "bad");
    }
  } finally {
    if (showMessage) {
      setBusy(false);
    }
  }
}

async function resolveOnlineName(name, size) {
  return onlineJson(`/api/devices/${encodeURIComponent(state.deviceId)}/books/resolve-name`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name, size }),
  });
}

async function uploadFileToOnlineLibrary(file) {
  try {
    if (!canUseOnlineLibrary()) {
      throw new Error(ONLINE_LIBRARY_API_BASE ? "Connect the device first." : "Online library is not configured.");
    }

    const upload = await prepareUpload(file);
    setBusy(true);
    showUploadProgress(0, "Online");
    const result = await uploadPreparedToOnlineLibrary(upload);
    showUploadProgress(1, "Complete");
    setStatus(`Added ${result.book.name} to online library.`, "good");
  } catch (error) {
    setStatus(error.message || "Online upload failed.", "bad");
  } finally {
    els.onlineFileInput.value = "";
    window.setTimeout(() => {
      els.uploadProgress.hidden = true;
    }, 900);
    setBusy(false);
  }
}

async function uploadPreparedToOnlineLibrary(upload) {
  const form = new FormData();
  form.append("name", upload.name);
  form.append("file", new Blob([upload.bytes], { type: "text/plain" }), upload.name);

  const result = await onlineJson(`/api/devices/${encodeURIComponent(state.deviceId)}/books`, {
    method: "POST",
    body: form,
  });
  updateOnlineLibrary(result);
  return result;
}

async function uploadOnlineBookToDevice(book) {
  try {
    if (!port) {
      throw new Error("Connect the device first.");
    }

    setBusy(true);
    setStatus(`Downloading ${book.name}...`);
    const response = await onlineFetch(`/api/devices/${encodeURIComponent(state.deviceId)}/books/${encodeURIComponent(book.id)}/content`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    const upload = {
      name: validateBookName(book.name),
      bytes,
    };

    await ensureStorageFresh();
    validateStorageForUpload(upload);
    await uploadPreparedToDevice(upload);
    setStatus("Online title uploaded to device.", "good");
  } catch (error) {
    setStatus(error.message || "Could not upload online title to device.", "bad");
  } finally {
    window.setTimeout(() => {
      els.uploadProgress.hidden = true;
    }, 900);
    setBusy(false);
  }
}

async function deleteOnlineBook(book) {
  if (!window.confirm(`Delete "${book.name}" from the online library?`)) {
    return;
  }

  try {
    setBusy(true);
    setStatus(`Deleting ${book.name} online...`);
    const data = await onlineJson(`/api/devices/${encodeURIComponent(state.deviceId)}/books/${encodeURIComponent(book.id)}`, {
      method: "DELETE",
    });
    updateOnlineLibrary(data);
    setStatus("Online title deleted.", "good");
  } catch (error) {
    setStatus(error.message || "Online delete failed.", "bad");
  } finally {
    setBusy(false);
  }
}

async function onlineFetch(path, options = {}) {
  if (!ONLINE_LIBRARY_API_BASE) {
    throw new Error("Online library is not configured.");
  }

  const response = await fetch(`${ONLINE_LIBRARY_API_BASE}${path}`, options);
  if (!response.ok) {
    let message = `Online server returned ${response.status}.`;
    try {
      const data = await response.clone().json();
      message = data.message || message;
    } catch (_) {
    }
    throw new Error(message);
  }

  return response;
}

async function onlineJson(path, options = {}) {
  const response = await onlineFetch(path, options);
  const data = await response.json();
  if (!data.ok) {
    throw new Error(data.message || "Online library rejected the request.");
  }
  return data;
}

async function prepareUpload(file) {
  const source = validateSourceFile(file);

  if (source.extension === "txt") {
    return {
      name: validateBookName(file.name),
      bytes: new Uint8Array(await file.arrayBuffer()),
    };
  }

  if (source.extension === "epub") {
    setBusy(true);
    setStatus("Converting EPUB to text...");
    showUploadProgress(0, "Converting");
    const text = await convertEpubToText(file);
    return makeTextUpload(file.name, text);
  }

  setBusy(true);
  setStatus("Converting PDF to text...");
  showUploadProgress(0, "Converting");
  const text = await convertPdfToText(file);
  return makeTextUpload(file.name, text);
}

function validateSourceFile(file) {
  const name = file.name.trim();
  const extension = fileExtension(name);

  if (!SUPPORTED_EXTENSIONS.has(extension)) {
    throw new Error("Only .txt, .epub, and text-based .pdf files can be uploaded.");
  }

  if (file.size <= 0) {
    throw new Error("Empty files are not supported.");
  }

  if (/[\\/:*?"<>|\x00-\x1f\x7f]/.test(name)) {
    throw new Error("Use a filename without path or system characters.");
  }

  return { name, extension };
}

function validateBookName(name) {
  const bookName = name.trim();

  if (!bookName.toLowerCase().endsWith(".txt")) {
    throw new Error("Converted files must end in .txt.");
  }

  if (bookName.length === 0 || bookName.length > 96) {
    throw new Error("Use a shorter filename.");
  }

  if (/[\\/:*?"<>|\x00-\x1f\x7f]/.test(bookName)) {
    throw new Error("Use a filename without path or system characters.");
  }

  return bookName;
}

function validateStorageForUpload(upload) {
  const storage = state.storage;
  if (!storage) {
    throw new Error("Storage details are not available yet.");
  }

  const existing = state.books.find((book) => book.name === upload.name);
  const replaceBytes = existing ? existing.size : 0;
  const available = storage.free + replaceBytes;

  if (upload.bytes.length > available) {
    throw new Error(`Not enough free storage. ${formatBytes(available)} is available for this file.`);
  }
}

function makeTextUpload(originalName, text) {
  const normalized = cleanText(text);
  if (normalized.length === 0) {
    throw new Error("No readable text was found in this file.");
  }

  return {
    name: validateBookName(txtNameFor(originalName)),
    bytes: encoder.encode(normalized),
  };
}

function fileExtension(name) {
  const dot = name.lastIndexOf(".");
  return dot >= 0 ? name.slice(dot + 1).toLowerCase() : "";
}

function txtNameFor(name) {
  const dot = name.lastIndexOf(".");
  const base = dot > 0 ? name.slice(0, dot) : name;
  return `${base}.txt`;
}

async function convertEpubToText(file) {
  if (!window.JSZip) {
    throw new Error("EPUB conversion library is not loaded.");
  }

  const zip = await window.JSZip.loadAsync(await file.arrayBuffer());
  const containerText = await readZipText(zip, "META-INF/container.xml");
  const containerDoc = parseXml(containerText, "EPUB container");
  const rootfile = firstByLocalName(containerDoc, "rootfile");
  const opfPath = rootfile?.getAttribute("full-path");

  if (!opfPath) {
    throw new Error("This EPUB is missing its package file.");
  }

  const opfText = await readZipText(zip, opfPath);
  const opfDoc = parseXml(opfText, "EPUB package");
  const opfBase = dirname(opfPath);
  const manifest = new Map();

  for (const item of allByLocalName(opfDoc, "item")) {
    const id = item.getAttribute("id");
    const href = item.getAttribute("href");
    if (id && href) {
      manifest.set(id, {
        href,
        mediaType: item.getAttribute("media-type") || "",
      });
    }
  }

  const sections = [];
  for (const itemref of allByLocalName(opfDoc, "itemref")) {
    const manifestItem = manifest.get(itemref.getAttribute("idref"));
    if (!manifestItem || !isHtmlMediaType(manifestItem.mediaType, manifestItem.href)) {
      continue;
    }

    const sectionPath = resolveZipPath(opfBase, manifestItem.href);
    const htmlText = await readZipText(zip, sectionPath);
    const htmlDoc = new DOMParser().parseFromString(htmlText, "text/html");
    const sectionText = extractReadableText(htmlDoc.body || htmlDoc);
    if (sectionText.length > 0) {
      sections.push(sectionText);
    }
  }

  if (sections.length === 0) {
    throw new Error("No readable chapters were found in this EPUB.");
  }

  return sections.join("\n\n");
}

async function convertPdfToText(file) {
  if (!window.pdfjsLib) {
    throw new Error("PDF conversion library is not loaded.");
  }

  const data = new Uint8Array(await file.arrayBuffer());
  const loadingTask = window.pdfjsLib.getDocument({
    data,
    disableWorker: window.location.protocol === "file:",
  });
  const pdf = await loadingTask.promise;
  const pages = [];

  for (let pageNumber = 1; pageNumber <= pdf.numPages; pageNumber += 1) {
    const page = await pdf.getPage(pageNumber);
    const content = await page.getTextContent();
    const pageText = extractPdfPageText(content.items || []);
    if (pageText.length > 0) {
      pages.push(pageText);
    }
    showUploadProgress(pageNumber / pdf.numPages, "Converting");
  }

  const text = cleanText(pages.join("\n\n"));
  const readableChars = text.replace(/\s/g, "").length;
  if (readableChars < PDF_TEXT_MIN_CHARS) {
    throw new Error("This PDF looks scanned or image-based. Please use a text-based PDF or EPUB.");
  }

  return text;
}

async function readZipText(zip, path) {
  const file = zip.file(path) || zip.file(decodeZipPath(path));
  if (!file) {
    throw new Error(`Missing EPUB file: ${path}`);
  }
  return file.async("text");
}

function parseXml(text, label) {
  const doc = new DOMParser().parseFromString(text, "application/xml");
  if (doc.getElementsByTagName("parsererror").length > 0) {
    throw new Error(`${label} could not be read.`);
  }
  return doc;
}

function firstByLocalName(root, localName) {
  return allByLocalName(root, localName)[0] || null;
}

function allByLocalName(root, localName) {
  return Array.from(root.getElementsByTagName("*")).filter((node) => node.localName === localName);
}

function isHtmlMediaType(mediaType, href) {
  const lowerType = mediaType.toLowerCase();
  const lowerHref = href.toLowerCase();
  return lowerType.includes("html") || lowerHref.endsWith(".html") || lowerHref.endsWith(".xhtml") || lowerHref.endsWith(".htm");
}

function resolveZipPath(base, href) {
  const withoutFragment = href.split("#")[0];
  const combined = base ? `${base}/${withoutFragment}` : withoutFragment;
  const parts = [];

  decodeZipPath(combined).split("/").forEach((part) => {
    if (!part || part === ".") {
      return;
    }
    if (part === "..") {
      parts.pop();
      return;
    }
    parts.push(part);
  });

  return parts.join("/");
}

function decodeZipPath(path) {
  try {
    return decodeURIComponent(path);
  } catch (_) {
    return path;
  }
}

function dirname(path) {
  const slash = path.lastIndexOf("/");
  return slash >= 0 ? path.slice(0, slash) : "";
}

function extractReadableText(root) {
  const blockTags = new Set(["ADDRESS", "ARTICLE", "ASIDE", "BLOCKQUOTE", "BR", "CAPTION", "CENTER", "DD", "DIV", "DL", "DT", "FIGCAPTION", "FIGURE", "FOOTER", "H1", "H2", "H3", "H4", "H5", "H6", "HEADER", "HR", "LI", "MAIN", "NAV", "OL", "P", "PRE", "SECTION", "TABLE", "TD", "TH", "TR", "UL"]);
  const ignoredTags = new Set(["SCRIPT", "STYLE", "SVG", "NOSCRIPT"]);
  const chunks = [];

  function walk(node) {
    if (!node) {
      return;
    }

    if (node.nodeType === Node.TEXT_NODE) {
      const text = node.nodeValue.replace(/\s+/g, " ").trim();
      if (text) {
        chunks.push(text);
      }
      return;
    }

    if (node.nodeType !== Node.ELEMENT_NODE || ignoredTags.has(node.tagName)) {
      return;
    }

    if (blockTags.has(node.tagName) && chunks.length > 0) {
      chunks.push("\n");
    }

    node.childNodes.forEach(walk);

    if (blockTags.has(node.tagName)) {
      chunks.push("\n");
    }
  }

  walk(root);
  return cleanText(chunks.join(" "));
}

function extractPdfPageText(items) {
  const lines = [];
  let currentY = null;
  let currentLine = [];

  function flushLine() {
    const line = currentLine.join(" ").replace(/\s+/g, " ").trim();
    if (line) {
      lines.push(line);
    }
    currentLine = [];
  }

  items.forEach((item) => {
    const text = (item.str || "").trim();
    if (!text) {
      return;
    }

    const y = Math.round(item.transform?.[5] || 0);
    if (currentY !== null && Math.abs(y - currentY) > 4) {
      flushLine();
    }

    currentY = y;
    currentLine.push(text);

    if (item.hasEOL) {
      flushLine();
      currentY = null;
    }
  });

  flushLine();
  return cleanText(lines.join("\n"));
}

function cleanText(text) {
  return text
    .replace(/\r/g, "\n")
    .replace(/[ \t]+\n/g, "\n")
    .replace(/\n[ \t]+/g, "\n")
    .replace(/[ \t]{2,}/g, " ")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
}

async function ensureStorageFresh() {
  if (!state.storage) {
    await refreshLibrary();
  }
}

function updateStorage(storage) {
  state.storage = storage;
  renderStorage();
}

function renderStorage() {
  const storage = state.storage || { used: 0, total: 0, free: 0 };
  const used = storage.used ?? 0;
  const total = storage.total ?? 0;
  const free = storage.free ?? Math.max(total - used, 0);
  const percent = total > 0 ? Math.min(100, (used / total) * 100) : 0;
  const usedParts = splitBytes(used);

  els.storageFill.style.width = `${percent}%`;
  els.usedNumber.textContent = usedParts.value;
  els.usedUnit.textContent = `${usedParts.unit} used`;
  els.totalBytes.textContent = `${formatBytes(total)} total`;
  els.freeBytes.textContent = `${formatBytes(free)} free`;
  els.storageDetail.textContent = `${percent.toFixed(1)}% capacity - ${state.books.length} ${state.books.length === 1 ? "title" : "titles"} stored`;
}

function updateBooks(books) {
  state.books = [...books].sort((a, b) => a.name.localeCompare(b.name));
  els.bookCount.textContent = `${state.books.length} ${state.books.length === 1 ? "Title" : "Titles"}`;
  els.bookRows.replaceChildren();

  if (state.books.length === 0) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 3;
    cell.className = "empty-row";
    cell.textContent = "No titles on device.";
    row.append(cell);
    els.bookRows.append(row);
    renderStorage();
    return;
  }

  state.books.forEach((book) => {
    const row = document.createElement("tr");

    const title = document.createElement("td");
    title.textContent = displayTitle(book.name);

    const size = document.createElement("td");
    size.textContent = formatBytes(book.size);

    const action = document.createElement("td");
    action.className = "action-cell";
    const button = document.createElement("button");
    button.type = "button";
    button.className = "row-action";
    button.textContent = "Delete";
    button.disabled = busy || !port;
    button.addEventListener("click", () => deleteBook(book));
    action.append(button);

    row.append(title, size, action);
    els.bookRows.append(row);
  });

  renderStorage();
}

function resetOnlineLibrary() {
  state.onlineStorage = null;
  state.onlineBooks = [];
  state.onlineAvailable = false;
  els.onlineLibrarySection.hidden = false;
  renderOnlineLibrary();
}

function updateOnlineLibrary(data) {
  state.onlineStorage = data.quota || null;
  state.onlineBooks = Array.isArray(data.books)
    ? [...data.books].sort((a, b) => a.name.localeCompare(b.name))
    : [];
  state.onlineAvailable = true;
  els.onlineLibrarySection.hidden = false;
  renderOnlineLibrary();
}

function renderOnlineLibraryMessage(message) {
  els.onlineLibrarySection.hidden = false;
  els.onlineBookRows.replaceChildren();
  const row = document.createElement("tr");
  const cell = document.createElement("td");
  cell.colSpan = 3;
  cell.className = "empty-row";
  cell.textContent = message;
  row.append(cell);
  els.onlineBookRows.append(row);
  els.onlineBookCount.textContent = "0 Titles";
  els.onlineStorageText.textContent = "Online unavailable";
  setBusy(false);
}

function renderOnlineLibrary() {
  const hasDevice = Boolean(state.deviceId);
  els.onlineBookCount.textContent = `${state.onlineBooks.length} ${state.onlineBooks.length === 1 ? "Title" : "Titles"}`;
  const quota = state.onlineStorage || { total: 0, used: 0, free: 0 };
  els.onlineStorageText.textContent = hasDevice ? `${formatBytes(quota.free || 0)} free online` : "Connect device";
  els.onlineBookRows.replaceChildren();

  if (state.onlineBooks.length === 0) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 3;
    cell.className = "empty-row";
    cell.textContent = state.onlineAvailable
      ? "No online titles yet."
      : hasDevice
        ? "Online library unavailable."
        : "Connect device to load online library.";
    row.append(cell);
    els.onlineBookRows.append(row);
    return;
  }

  state.onlineBooks.forEach((book) => {
    const row = document.createElement("tr");

    const title = document.createElement("td");
    title.textContent = displayTitle(book.name);

    const size = document.createElement("td");
    size.textContent = formatBytes(book.size);

    const action = document.createElement("td");
    action.className = "action-cell online-row-actions";

    const uploadButton = document.createElement("button");
    uploadButton.type = "button";
    uploadButton.className = "row-action online-upload-action";
    uploadButton.textContent = "Upload";
    uploadButton.disabled = busy || !port || !state.onlineAvailable;
    uploadButton.addEventListener("click", () => uploadOnlineBookToDevice(book));

    const deleteButton = document.createElement("button");
    deleteButton.type = "button";
    deleteButton.className = "row-action";
    deleteButton.textContent = "Delete";
    deleteButton.disabled = busy || !state.onlineAvailable;
    deleteButton.addEventListener("click", () => deleteOnlineBook(book));

    action.append(uploadButton, deleteButton);
    row.append(title, size, action);
    els.onlineBookRows.append(row);
  });
}

function displayTitle(name) {
  return name.replace(/\.txt$/i, "");
}

function showUploadProgress(fraction, label) {
  const percent = Math.max(0, Math.min(1, fraction)) * 100;
  els.uploadProgress.hidden = false;
  els.uploadFill.style.width = `${percent}%`;
  els.uploadText.textContent = label;
}

function textToBase64(text) {
  return bytesToBase64(encoder.encode(text));
}

function bytesToBase64(bytes) {
  let binary = "";
  for (let index = 0; index < bytes.length; index += 1) {
    binary += String.fromCharCode(bytes[index]);
  }
  return btoa(binary);
}

function formatBytes(value) {
  if (!Number.isFinite(value) || value <= 0) {
    return "0 B";
  }

  const units = ["B", "KB", "MB", "GB"];
  let size = value;
  let unitIndex = 0;

  while (size >= 1024 && unitIndex < units.length - 1) {
    size /= 1024;
    unitIndex += 1;
  }

  const digits = size >= 10 || unitIndex === 0 ? 0 : 1;
  return `${size.toFixed(digits)} ${units[unitIndex]}`;
}

function splitBytes(value) {
  const formatted = formatBytes(value);
  const splitAt = formatted.lastIndexOf(" ");
  return {
    value: formatted.slice(0, splitAt),
    unit: formatted.slice(splitAt + 1),
  };
}

function wait(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

async function rejectAfter(promise, timeoutMs, message) {
  let timeoutId = 0;

  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timeoutId = window.setTimeout(() => {
          reject(new Error(message));
        }, timeoutMs);
      }),
    ]);
  } finally {
    window.clearTimeout(timeoutId);
  }
}

async function settleWithin(promise, timeoutMs) {
  let timeoutId = 0;

  try {
    await Promise.race([
      promise.catch(() => {}),
      new Promise((resolve) => {
        timeoutId = window.setTimeout(resolve, timeoutMs);
      }),
    ]);
  } finally {
    window.clearTimeout(timeoutId);
  }
}

function startHeartbeat() {
  stopHeartbeat();
  heartbeatTimer = window.setInterval(() => {
    if (!port || busy || pendingResponses.length > 0) {
      return;
    }
    sendCommand("ACAT PING", ["ping"], 3000).catch(() => {});
  }, 15000);
}

function stopHeartbeat() {
  if (heartbeatTimer) {
    window.clearInterval(heartbeatTimer);
    heartbeatTimer = 0;
  }
}

renderStorage();
resetOnlineLibrary();
setBusy(false);
