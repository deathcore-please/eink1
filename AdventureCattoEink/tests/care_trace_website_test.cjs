const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const { pathToFileURL } = require("node:url");
const { chromium } = require("playwright");

(async () => {
  const output = await fs.mkdtemp(path.join(os.tmpdir(), "AdventureCattoTraceUI-"));
  const browser = await chromium.launch({ headless: true, channel: process.env.PLAYWRIGHT_CHANNEL || "chrome" });
  try {
    for (const viewport of [{ width: 1440, height: 1000 }, { width: 375, height: 812 }]) {
      const page = await browser.newPage({ viewport, acceptDownloads: true });
      const errors = [];
      page.on("pageerror", (error) => errors.push(error.message));
      await page.addInitScript(() => {
        Object.defineProperty(navigator, "serial", { value: { getPorts: async () => [], addEventListener() {} } });
      });
      await page.goto(pathToFileURL(path.resolve(__dirname, "../website/index.html")).href);
      await page.waitForFunction(() => typeof renderDiagnostics === "function");
      assert.equal(await page.locator("#diagnosticsSection").isVisible(), false);
      await page.evaluate(() => {
        port = {};
        state.deviceId = "test-device";
        window.testCommands = [];
        window.testCleared = false;
        const log = '{"event":"drain","before":{"happiness":80},"after":{"happiness":0}}\n';
        window.testLog = log;
        sendCommand = async (command) => {
          testCommands.push(command);
          if (command.includes("CARETRACE READ")) {
            const offset = Number(command.split(" ").at(-1));
            const chunk = log.slice(offset, offset + 25);
            return { bytes: log.length, chunk: btoa(chunk), nextOffset: offset + chunk.length,
                     done: offset + chunk.length >= log.length };
          }
          if (command.includes("CARETRACE CLEAR")) { testCleared = true; return { bytes: 0, entries: 0 }; }
          if (command.includes("CARETRACE INFO")) return { bytes: testCleared ? 0 : log.length, entries: testCleared ? 0 : 1 };
          throw new Error(`Unexpected diagnostics command: ${command}`);
        };
        updateDeathLogInfo({ bytes: 30, entries: 1 });
        state.careTrace = { bytes: log.length, entries: 1 };
        renderDiagnostics();
      });
      const diagnostics = page.locator("#diagnosticsSection");
      await diagnostics.scrollIntoViewIfNeeded();
      await page.screenshot({ path: path.join(output, `trace-${viewport.width}.png`), fullPage: true });
      const bounds = await page.locator(".care-trace-actions").evaluate((element) => {
        const parent = element.getBoundingClientRect();
        return [...element.children].map((child) => {
          const box = child.getBoundingClientRect();
          return { contained: box.left >= parent.left - 1 && box.right <= parent.right + 1,
                   fits: child.scrollWidth <= child.clientWidth + 1 };
        });
      });
      assert.ok(bounds.every((box) => box.contained && box.fits), "diagnostic controls overflow");
      await page.evaluate(() => setBusy(true));
      assert.equal(await page.locator("#downloadCareTraceButton").isDisabled(), true);
      assert.equal(await page.locator("#clearCareTraceButton").isDisabled(), true);
      await page.evaluate(() => setBusy(false));
      const downloadPromise = page.waitForEvent("download");
      await page.locator("#downloadCareTraceButton").click();
      const download = await downloadPromise;
      assert.equal(download.suggestedFilename(), "test-device-care-trace.jsonl");
      const downloaded = await fs.readFile(await download.path(), "utf8");
      assert.equal(downloaded, await page.evaluate(() => testLog));
      assert.equal(JSON.parse(downloaded).after.happiness, 0);
      await page.waitForFunction(() => !busy);
      page.once("dialog", (dialog) => dialog.accept());
      await page.locator("#clearCareTraceButton").click();
      await page.waitForFunction(() => !busy && testCleared);
      assert.equal(await page.locator("#downloadCareTraceButton").isDisabled(), true);
      assert.equal(await page.locator("#downloadDeathLogButton").isDisabled(), false);
      assert.ok((await page.evaluate(() => testCommands)).every((command) => command.startsWith("ACAT DIAG CARETRACE")));
      await page.evaluate(() => { port = null; resetDiagnostics(); });
      assert.equal(await diagnostics.isVisible(), false);
      assert.deepEqual(errors, []);
      await page.close();
      console.log(`PASS: ${viewport.width}px USB-only trace download, clear, busy/disconnected controls and layout`);
    }
    const cachedPage = await browser.newPage();
    const cachedErrors = [];
    cachedPage.on("pageerror", (error) => cachedErrors.push(error.message));
    await cachedPage.addInitScript(() => {
      Object.defineProperty(navigator, "serial", { value: { getPorts: async () => [], addEventListener() {} } });
      const getElement = document.getElementById.bind(document);
      document.getElementById = (id) => ["careTraceInfo", "downloadCareTraceButton", "clearCareTraceButton"].includes(id)
        ? null : getElement(id);
    });
    await cachedPage.goto(pathToFileURL(path.resolve(__dirname, "../website/index.html")).href);
    await cachedPage.waitForFunction(() => typeof renderDiagnostics === "function");
    await cachedPage.evaluate(() => { port = {}; updateDeathLogInfo({ bytes: 30, entries: 1 }); setBusy(false); });
    assert.deepEqual(cachedErrors, []);
    assert.equal(await cachedPage.locator("#downloadDeathLogButton").isDisabled(), false);
    await cachedPage.close();
    console.log("PASS: cached older HTML without care-trace controls still initializes and retains death-log controls");
    console.log(`Screenshots: ${output}`);
  } finally {
    await browser.close();
  }
})().catch((error) => { console.error(error); process.exitCode = 1; });
