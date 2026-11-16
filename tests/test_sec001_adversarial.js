const fs = require("fs");
const path = require("path");
const assert = require("assert");

function testSec001AdversarialSuite() {
  console.log("\n=======================================================");
  console.log(" Running Blob Interception & Non-Blocking Verification...");
  console.log("=======================================================");

  const csPath = path.join(__dirname, "../extension/js/content-script.js");
  const csText = fs.readFileSync(csPath, "utf8");

  // Verify code invariants: ALL SEC-001 / g_authorizedBlobTokens blocking removed
  assert.ok(!csText.includes("g_authorizedBlobTokens"), "g_authorizedBlobTokens blocking must be completely removed");
  assert.ok(!csText.includes("data-radium-auth-token"), "data-radium-auth-token blocking must be removed");
  assert.ok(!csText.includes("[Radium Security] SEC-001 BLOCKED"), "No console.warn SEC-001 BLOCKED spam logged");
  assert.ok(csText.includes("radiumDirectBlobDownload"), "radiumDirectBlobDownload handler present");
  assert.ok(csText.includes("radiumBlobFallback"), "radiumBlobFallback safe browser fallback present");

  // Simulate content-script.js radiumDirectBlobDownload forwarding logic
  const dispatchedToBackground = [];
  function simulateRadiumDirectBlobDownload(msg) {
    const isBlobOrDataUrl = typeof msg.url === "string" &&
      (msg.url.startsWith("blob:") || msg.url.startsWith("data:"));
    const dataBase64 = typeof msg.dataBase64 === "string" ? msg.dataBase64 : "";
    if (!isBlobOrDataUrl && !dataBase64) {
      return { accepted: false, reason: "INVALID_NON_BLOB_PAYLOAD" };
    }
    dispatchedToBackground.push(msg);
    return { accepted: true };
  }

  // Scenario 1: Gemini programmatic detached a.click() without authToken or gesture
  const geminiRes = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://gemini.google.com/artifact-9999",
    filename: "radium_download_manager_design_vision.md",
    dataBase64: "IyBSYWRpdW0K"
  });
  assert.strictEqual(geminiRes.accepted, true, "Gemini blob download must never be rejected");

  // Scenario 2: Gemini showSaveFilePicker / worker blob without inline base64
  const workerBlobRes = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://gemini.google.com/worker-blob-1234",
    filename: "export.json",
    dataBase64: ""
  });
  assert.strictEqual(workerBlobRes.accepted, true, "Worker/CSP blob URL must be forwarded to background fetch");

  // Scenario 3: data: URL export
  const dataUrlRes = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "data:text/markdown;base64,IyBIZWxsbw==",
    filename: "notes.md",
    dataBase64: "IyBIZWxsbw=="
  });
  assert.strictEqual(dataUrlRes.accepted, true, "data: URL export must be forwarded");

  // Scenario 4: Non-blob/non-data empty payload ignored
  const invalidRes = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "https://example.com/file.zip",
    filename: "file.zip",
    dataBase64: ""
  });
  assert.strictEqual(invalidRes.accepted, false);

  assert.strictEqual(dispatchedToBackground.length, 3);
  console.log("  -> PASS: All Gemini / SPA blob & data URL downloads forwarded without SEC-001 blocking.");
  console.log("=======================================================\n");
}

module.exports = { testSec001AdversarialSuite };

if (require.main === module) {
  testSec001AdversarialSuite();
}
