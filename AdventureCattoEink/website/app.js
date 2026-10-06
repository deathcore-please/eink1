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
let activeUploadJob = null;

const pendingResponses = [];
const state = {
  deviceId: "",
  storage: null,
  books: [],
  deathLog: null,
  careTrace: null,
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
  diagnosticsSection: document.getElementById("diagnosticsSection"),
  deathLogInfo: document.getElementById("deathLogInfo"),
  downloadDeathLogButton: document.getElementById("downloadDeathLogButton"),
  clearDeathLogButton: document.getElementById("clearDeathLogButton"),
  careTraceInfo: document.getElementById("careTraceInfo"),
  downloadCareTraceButton: document.getElementById("downloadCareTraceButton"),
  clearCareTraceButton: document.getElementById("clearCareTraceButton"),
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
  cancelUploadButton: document.getElementById("cancelUploadButton"),
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

els.cancelUploadButton.addEventListener("click", () => {
  requestActiveUploadCancel();
});

els.downloadDeathLogButton.addEventListener("click", () => {
  downloadDeathLog();
});

els.clearDeathLogButton.addEventListener("click", () => {
  clearDeathLog();
});
els.downloadCareTraceButton?.addEventListener("click", () => downloadDiagnosticLog("CARETRACE"));
els.clearCareTraceButton?.addEventListener("click", () => clearCareTrace());

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
  updateDiagnosticsControls();
  updateCancelUploadButton();
}

class UploadCancelledError extends Error {
  constructor() {
    super("Upload cancelled.");
    this.name = "UploadCancelledError";
  }
}

function isUploadCancelledError(error) {
  return error?.name === "UploadCancelledError";
}

function createUploadJob(kind) {
  if (activeUploadJob) {
    throw new Error("Another upload is already active.");
  }

  activeUploadJob = {
    kind,
    uploadId: createUploadId(),
    cancelRequested: false,
    cleanupStarted: false,
    deviceName: "",
    deviceUploadStarted: false,
    deviceCommitted: false,
    onlineUploadStarted: false,
    onlineCommitted: false,
    onlineFileId: "",
    progress: 0,
  };
  updateCancelUploadButton();
  return activeUploadJob;
}

function finishUploadJob(job) {
  if (activeUploadJob === job) {
    activeUploadJob = null;
  }
  updateCancelUploadButton();
}

function requestActiveUploadCancel() {
  if (!activeUploadJob || activeUploadJob.cleanupStarted) {
    return;
  }

  activeUploadJob.cancelRequested = true;
  setStatus("Cancelling upload...");
  showUploadProgress(activeUploadJob.progress || 0, "Cancelling");
  updateCancelUploadButton();
}

function updateCancelUploadButton() {
  if (!els.cancelUploadButton) {
    return;
  }

  const hasActiveJob = Boolean(activeUploadJob);
  els.cancelUploadButton.disabled = !hasActiveJob || activeUploadJob.cleanupStarted;
  els.cancelUploadButton.textContent = activeUploadJob?.cancelRequested ? "Cancelling" : "Cancel";
}

function throwIfUploadCancelled(job) {
  if (job?.cancelRequested) {
    throw new UploadCancelledError();
  }
}

function createUploadId() {
  if (crypto.randomUUID) {
    return `upl-${crypto.randomUUID()}`;
  }

  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
  return `upl-${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function setConnected(isConnected, keepBusy = false) {
  els.connectionState.classList.toggle("connected", isConnected);
  els.connectionState.lastChild.textContent = isConnected ? " Connected" : " Disconnected";
  els.connectButton.textContent = isConnected ? "Disconnect" : "Connect";
  renderPortOptions();
  renderDiagnostics();
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
    await refreshDeathLogInfo(false);
    await refreshCareTraceInfo();
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
  resetDiagnostics();
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

async function refreshDeathLogInfo(showMessage = true) {
  if (!port) {
    resetDiagnostics();
    return;
  }

  try {
    if (showMessage) {
      setBusy(true);
      setStatus("Reading diagnostics...");
    }

    const data = await sendCommand("ACAT DIAG DEATHLOG INFO", ["diag-deathlog-info"], 8000);
    updateDeathLogInfo(data);

    if (showMessage) {
      setStatus("Diagnostics refreshed.", "good");
    }
  } catch (error) {
    state.deathLog = null;
    renderDiagnostics();
    if (showMessage) {
      setStatus(error.message || "Could not read diagnostics.", "bad");
    }
  } finally {
    if (showMessage) {
      setBusy(false);
    }
  }
}

async function downloadDeathLog() {
  return downloadDiagnosticLog("DEATHLOG");
}

async function refreshCareTraceInfo() {
  if (!port) return;
  try {
    const data = await sendCommand("ACAT DIAG CARETRACE INFO", ["diag-caretrace-info"], 8000);
    state.careTrace = { bytes: Number(data.bytes || 0), entries: Number(data.entries || 0) };
  } catch {
    state.careTrace = null;
  }
  renderDiagnostics();
}

async function clearCareTrace() {
  if (!port || busy || !window.confirm("Clear the care trace from the device?")) return;
  try {
    setBusy(true);
    await sendCommand("ACAT DIAG CARETRACE CLEAR", ["diag-caretrace-clear"], 8000);
    await refreshCareTraceInfo();
    setStatus("Care trace cleared.", "good");
  } catch (error) {
    setStatus(error.message || "Could not clear care trace.", "bad");
  } finally {
    setBusy(false);
  }
}

async function downloadDiagnosticLog(topic) {
  const careTrace = topic === "CARETRACE";
  const label = careTrace ? "care trace" : "death log";
  const responseType = careTrace ? "diag-caretrace-read" : "diag-deathlog-read";
  if (!port) {
    setStatus("Connect the device first.", "bad");
    return;
  }

  try {
    setBusy(true);
    setStatus(`Downloading ${label}...`);

    const chunks = [];
    let offset = 0;
    let totalBytes = 0;

    while (true) {
      const response = await sendCommand(`ACAT DIAG ${topic} READ ${offset}`, [responseType], 10000);
      totalBytes = Number(response.bytes || 0);

      if (response.chunk) {
        chunks.push(base64ToBytes(response.chunk));
      }

      const nextOffset = Number(response.nextOffset || offset);
      if (!response.done && nextOffset <= offset) {
        throw new Error("Device diagnostics transfer stalled.");
      }
      offset = nextOffset;

      if (response.done) {
        break;
      }
    }

    if (totalBytes <= 0) {
      setStatus(`No ${label} is stored on the device.`, "good");
      if (careTrace) await refreshCareTraceInfo();
      else updateDeathLogInfo({ bytes: 0, entries: 0 });
      return;
    }

    const blob = new Blob(chunks, { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `${state.deviceId || "adventure-catto"}-${careTrace ? "care-trace.jsonl" : "death-log.txt"}`;
    document.body.append(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 5000);

    if (careTrace) await refreshCareTraceInfo();
    else await refreshDeathLogInfo(false);
    setStatus(`${careTrace ? "Care trace" : "Death log"} downloaded.`, "good");
  } catch (error) {
    setStatus(error.message || `${label} download failed.`, "bad");
  } finally {
    setBusy(false);
  }
}

async function clearDeathLog() {
  if (!port) {
    setStatus("Connect the device first.", "bad");
    return;
  }

  if (!window.confirm("Clear the death diagnostic log from the device?")) {
    return;
  }

  try {
    setBusy(true);
    setStatus("Clearing death log...");
    const data = await sendCommand("ACAT DIAG DEATHLOG CLEAR", ["diag-deathlog-clear"], 8000);
    updateDeathLogInfo(data);
    setStatus("Death log cleared.", "good");
  } catch (error) {
    setStatus(error.message || "Could not clear death log.", "bad");
  } finally {
    setBusy(false);
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
  let job = null;
  try {
    if (!port) {
      throw new Error("Connect the device first.");
    }

    job = createUploadJob("direct");
    setBusy(true);
    setStatus("Preparing upload...");
    showUploadProgress(0, "Preparing");

    let upload = await prepareUpload(file, job);
    let onlineSyncMessage = "";
    let shouldSyncOnline = true;

    await ensureStorageFresh();
    throwIfUploadCancelled(job);

    const namePlan = await chooseUploadName(upload.name, upload.bytes.length, canUseOnlineLibrary(), job);
    upload = {
      ...upload,
      name: namePlan.name,
    };
    shouldSyncOnline = namePlan.shouldSyncOnline;
    onlineSyncMessage = namePlan.onlineSyncMessage;

    validateStorageForUpload(upload);
    throwIfUploadCancelled(job);

    await uploadPreparedToDevice(upload, job);
    throwIfUploadCancelled(job);

    if (canUseOnlineLibrary() && shouldSyncOnline) {
      try {
        await uploadPreparedToOnlineLibrary(upload, job);
      } catch (error) {
        if (job.cancelRequested || isUploadCancelledError(error)) {
          throw error;
        }
        onlineSyncMessage = ` Online server is down: ${error.message || "backup failed"}.`;
      }
    }

    throwIfUploadCancelled(job);
    setStatus(`Title uploaded.${onlineSyncMessage}`, onlineSyncMessage ? "bad" : "good");
  } catch (error) {
    if (job && (job.cancelRequested || isUploadCancelledError(error))) {
      await handleCancelledUpload(job);
    } else {
      setStatus(error.message || "Upload failed.", "bad");
    }
  } finally {
    els.fileInput.value = "";
    window.setTimeout(() => {
      els.uploadProgress.hidden = true;
    }, 900);
    finishUploadJob(job);
    setBusy(false);
  }
}

async function chooseUploadName(initialName, size, allowOnline, job) {
  let name = uniqueBookNameFor(initialName, state.books);
  let shouldSyncOnline = allowOnline;
  let onlineSyncMessage = "";

  if (!allowOnline) {
    return { name, shouldSyncOnline: false, onlineSyncMessage };
  }

  try {
    for (let attempt = 0; attempt < 8; attempt += 1) {
      throwIfUploadCancelled(job);
      const resolved = await resolveOnlineName(name, size);
      throwIfUploadCancelled(job);

      if (resolved.canStore === false) {
        return {
          name,
          shouldSyncOnline: false,
          onlineSyncMessage: " Online library is full; uploaded to device only.",
        };
      }

      const deviceSafeName = uniqueBookNameFor(resolved.name, state.books);
      if (deviceSafeName === resolved.name) {
        return { name: resolved.name, shouldSyncOnline, onlineSyncMessage };
      }
      name = deviceSafeName;
    }

    return { name, shouldSyncOnline, onlineSyncMessage };
  } catch (error) {
    if (job.cancelRequested || isUploadCancelledError(error)) {
      throw error;
    }
    return {
      name,
      shouldSyncOnline: false,
      onlineSyncMessage: ` Online server is down: ${error.message || "could not reserve name"}.`,
    };
  }
}

async function uploadPreparedToDevice(upload, job) {
  let uploadStarted = false;

  try {
    setBusy(true);
    job.deviceName = upload.name;
    job.progress = 0;
    showUploadProgress(0, "Starting");
    throwIfUploadCancelled(job);

    await sendCommand(`ACAT BEGIN ${textToBase64(upload.name)} ${upload.bytes.length}`, ["begin"], 10000);
    uploadStarted = true;
    job.deviceUploadStarted = true;
    throwIfUploadCancelled(job);

    const bytes = upload.bytes;
    let offset = 0;

    while (offset < bytes.length) {
      throwIfUploadCancelled(job);
      const chunk = bytes.slice(offset, offset + CHUNK_SIZE);
      await sendCommand(`ACAT DATA ${bytesToBase64(chunk)}`, ["data"], 12000);
      offset += chunk.length;
      job.progress = offset / bytes.length;
      showUploadProgress(job.progress, `${Math.round(job.progress * 100)}%`);
      throwIfUploadCancelled(job);
    }

    await sendCommand("ACAT END", ["list"], 15000);
    job.deviceCommitted = true;
    job.progress = 1;
    showUploadProgress(1, "Complete");
  } catch (error) {
    if (uploadStarted && !job.cancelRequested && !isUploadCancelledError(error)) {
      sendCommand("ACAT CANCEL", ["cancel"], 3000).catch(() => {});
    }
    throw error;
  }
}

async function handleCancelledUpload(job) {
  try {
    await cleanupCancelledUpload(job);
    setStatus("Upload cancelled.", "good");
  } catch (error) {
    setStatus(error.message || "Upload cancelled, but cleanup failed.", "bad");
  }
}

async function cleanupCancelledUpload(job) {
  if (!job || job.cleanupStarted) {
    return;
  }

  job.cleanupStarted = true;
  updateCancelUploadButton();
  setStatus("Cancelling upload...");
  showUploadProgress(job.progress || 0, "Cancelling");

  const errors = [];

  if (job.deviceUploadStarted && !job.deviceCommitted) {
    try {
      await sendCommand("ACAT CANCEL", ["cancel"], 5000);
    } catch (error) {
      errors.push(error.message || "device partial cleanup failed");
    }
  } else if (job.deviceCommitted && job.deviceName) {
    try {
      await sendCommand(`ACAT DELETE ${textToBase64(job.deviceName)}`, ["list"], 10000);
    } catch (error) {
      errors.push(error.message || "device cleanup failed");
    }
  }

  if (job.onlineUploadStarted && job.uploadId && canUseOnlineLibrary()) {
    try {
      const data = await onlineJson(`/api/devices/${encodeURIComponent(state.deviceId)}/uploads/${encodeURIComponent(job.uploadId)}`, {
        method: "DELETE",
      });
      updateOnlineLibrary(data);
    } catch (error) {
      errors.push(error.message || "online cleanup failed");
    }
  }

  if (errors.length > 0) {
    throw new Error(`Upload cancelled, but cleanup needs attention: ${errors.join("; ")}.`);
  }
}

async function uploadFileToOnlineLibrary(file) {
  let job = null;
  try {
    if (!canUseOnlineLibrary()) {
      throw new Error(ONLINE_LIBRARY_API_BASE ? "Connect the device first." : "Online library is not configured.");
    }

    job = createUploadJob("online-only");
    setBusy(true);
    setStatus("Preparing online upload...");
    showUploadProgress(0, "Preparing");
    const upload = await prepareUpload(file, job);
    throwIfUploadCancelled(job);
    showUploadProgress(0, "Online");
    const result = await uploadPreparedToOnlineLibrary(upload, job);
    throwIfUploadCancelled(job);
    showUploadProgress(1, "Complete");
    setStatus(`Added ${result.book.name} to online library.`, "good");
  } catch (error) {
    if (job && (job.cancelRequested || isUploadCancelledError(error))) {
      await handleCancelledUpload(job);
    } else {
      setStatus(error.message || "Online upload failed.", "bad");
    }
  } finally {
    els.onlineFileInput.value = "";
    window.setTimeout(() => {
      els.uploadProgress.hidden = true;
    }, 900);
    finishUploadJob(job);
    setBusy(false);
  }
}

async function uploadPreparedToOnlineLibrary(upload, job) {
  const uploadId = job?.uploadId || createUploadId();
  const form = new FormData();
  form.append("name", upload.name);
  form.append("uploadId", uploadId);
  form.append("file", new Blob([upload.bytes], { type: "text/plain" }), upload.name);

  if (job) {
    job.onlineUploadStarted = true;
    job.progress = Math.max(job.progress || 0, 0.98);
    showUploadProgress(job.progress, "Online");
  }

  const result = await onlineJson(`/api/devices/${encodeURIComponent(state.deviceId)}/books`, {
    method: "POST",
    body: form,
  });

  if (job) {
    job.onlineCommitted = true;
    job.onlineFileId = result.book?.id || "";
  }

  throwIfUploadCancelled(job);
  updateOnlineLibrary(result);
  return result;
}

async function uploadOnlineBookToDevice(book) {
  let job = null;
  try {
    if (!port) {
      throw new Error("Connect the device first.");
    }

    job = createUploadJob("online-to-device");
    setBusy(true);
    setStatus(`Downloading ${book.name}...`);
    showUploadProgress(0, "Downloading");
    const response = await onlineFetch(`/api/devices/${encodeURIComponent(state.deviceId)}/books/${encodeURIComponent(book.id)}/content`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    throwIfUploadCancelled(job);

    await ensureStorageFresh();
    const upload = {
      name: uniqueBookNameFor(validateBookName(book.name), state.books),
      bytes,
    };

    validateStorageForUpload(upload);
    throwIfUploadCancelled(job);
    await uploadPreparedToDevice(upload, job);
    throwIfUploadCancelled(job);
    setStatus("Online title uploaded to device.", "good");
  } catch (error) {
    if (job && (job.cancelRequested || isUploadCancelledError(error))) {
      await handleCancelledUpload(job);
    } else {
      setStatus(error.message || "Could not upload online title to device.", "bad");
    }
  } finally {
    window.setTimeout(() => {
      els.uploadProgress.hidden = true;
    }, 900);
    finishUploadJob(job);
    setBusy(false);
  }
}

function uniqueBookNameFor(name, books) {
  const existing = new Set(books.map((book) => book.name.toLowerCase()));
  const original = validateBookName(name);
  if (!existing.has(original.toLowerCase())) {
    return original;
  }

  const extension = ".txt";
  const base = original.slice(0, -extension.length);
  let index = 1;

  while (true) {
    const suffix = ` (${index})${extension}`;
    const candidate = validateBookName(`${base.slice(0, 96 - suffix.length)}${suffix}`);
    if (!existing.has(candidate.toLowerCase())) {
      return candidate;
    }
    index += 1;
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

async function prepareUpload(file, job) {
  const source = validateSourceFile(file);
  throwIfUploadCancelled(job);

  if (source.extension === "txt") {
    const bytes = new Uint8Array(await file.arrayBuffer());
    throwIfUploadCancelled(job);
    return {
      name: validateBookName(file.name),
      bytes,
    };
  }

  if (source.extension === "epub") {
    setBusy(true);
    setStatus("Converting EPUB to text...");
    showUploadProgress(0, "Converting");
    const text = await convertEpubToText(file, job);
    throwIfUploadCancelled(job);
    return makeTextUpload(file.name, text);
  }

  setBusy(true);
  setStatus("Converting PDF to text...");
  showUploadProgress(0, "Converting");
  const text = await convertPdfToText(file, job);
  throwIfUploadCancelled(job);
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
  if (existing) {
    throw new Error("A title with this name already exists on the device.");
  }

  const available = storage.free;

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

async function convertEpubToText(file, job) {
  if (!window.JSZip) {
    throw new Error("EPUB conversion library is not loaded.");
  }

  const zip = await window.JSZip.loadAsync(await file.arrayBuffer());
  throwIfUploadCancelled(job);
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
    throwIfUploadCancelled(job);
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

async function convertPdfToText(file, job) {
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
    throwIfUploadCancelled(job);
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
  if (!port) {
    if (!state.storage) {
      throw new Error("Storage details are not available yet.");
    }
    return;
  }

  await sendCommand("ACAT LIST", ["list"], 8000);
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

function resetDiagnostics() {
  state.deathLog = null;
  state.careTrace = null;
  renderDiagnostics();
}

function updateDeathLogInfo(data) {
  state.deathLog = {
    bytes: Number(data.bytes || 0),
    entries: Number(data.entries || 0),
  };
  renderDiagnostics();
}

function renderDiagnostics() {
  const connected = Boolean(port);
  els.diagnosticsSection.hidden = !connected;

  if (!connected) {
    return;
  }

  if (!state.deathLog) {
    els.deathLogInfo.textContent = "Unavailable";
  } else {
    const entries = state.deathLog.entries || 0;
    els.deathLogInfo.textContent = `${entries} ${entries === 1 ? "Record" : "Records"} - ${formatBytes(state.deathLog.bytes || 0)}`;
  }

  updateDiagnosticsControls();
}

function updateDiagnosticsControls() {
  if (!els.downloadDeathLogButton || !els.clearDeathLogButton) {
    return;
  }

  const hasLog = Boolean(port && state.deathLog && state.deathLog.bytes > 0);
  els.downloadDeathLogButton.disabled = busy || !hasLog;
  els.clearDeathLogButton.disabled = busy || !hasLog;
  if (!els.downloadCareTraceButton || !els.clearCareTraceButton || !els.careTraceInfo) return;
  const hasTrace = Boolean(port && state.careTrace && state.careTrace.bytes > 0);
  els.downloadCareTraceButton.disabled = busy || !hasTrace;
  els.clearCareTraceButton.disabled = busy || !hasTrace;
  els.careTraceInfo.textContent = state.careTrace
    ? `${state.careTrace.entries} ${state.careTrace.entries === 1 ? "Record" : "Records"} - ${formatBytes(state.careTrace.bytes)}`
    : "Unavailable";
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

function base64ToBytes(encoded) {
  const binary = atob(encoded || "");
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
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
resetDiagnostics();
resetOnlineLibrary();
setBusy(false);
