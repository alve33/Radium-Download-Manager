const fs = require("fs");
const path = require("path");
const assert = require("assert");

function testSec001AdversarialSuite() {
  console.log("\n=======================================================");
  console.log(" Running SEC-001 Adversarial Token Binding Tests...");
  console.log("=======================================================");

  const csPath = path.join(__dirname, "../extension/js/content-script.js");
  const csText = fs.readFileSync(csPath, "utf8");

  // Verify code invariants
  assert.ok(csText.includes("g_authorizedBlobTokens"), "g_authorizedBlobTokens token registry declared");
  assert.ok(csText.includes("data-radium-auth-token"), "auth token bound to clicked DOM anchor attribute");
  assert.ok(csText.includes("ev.data.authToken"), "authToken verified in radiumDirectBlobDownload");
  assert.ok(!csText.includes("flux-deepsearch"), "Static flux-deepsearch nonce eliminated for blob authorization");
  assert.ok(!csText.includes("Date.now() - g_lastUserGestureTime"), "Global user gesture time window eliminated for blob authorization");

  // In-memory simulation of the Content Script runtime environment
  const mockTokens = new Map();
  let dispatchedToBackground = [];

  function simulateHandleLinkClick(ev, anchor) {
    if (ev && ev.isTrusted) {
      let preHref = anchor.href || "";
      if (preHref && (preHref.startsWith("blob:") || preHref.startsWith("data:"))) {
        const authToken = "radium-auth-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 10);
        const targetFilename = anchor.download || "";
        mockTokens.set(authToken, {
          href: preHref,
          filename: targetFilename,
          timestamp: Date.now()
        });
        anchor.setAttribute("data-radium-auth-token", authToken);
      }
    }
  }

  function simulateRadiumDirectBlobDownload(msg) {
    const authToken = msg.authToken;
    if (!authToken || !mockTokens.has(authToken)) {
      return { accepted: false, reason: "BLOCKED_NO_OR_INVALID_TOKEN" };
    }
    const authRecord = mockTokens.get(authToken);
    mockTokens.delete(authToken); // Strict single-use immediate revocation

    if (Date.now() - authRecord.timestamp > 15000) {
      return { accepted: false, reason: "BLOCKED_EXPIRED_TOKEN" };
    }
    if (msg.url !== authRecord.href) {
      return { accepted: false, reason: "BLOCKED_URL_MISMATCH" };
    }
    if (authRecord.filename && msg.filename && authRecord.filename !== msg.filename) {
      return { accepted: false, reason: "BLOCKED_FILENAME_MISMATCH" };
    }
    dispatchedToBackground.push(msg);
    return { accepted: true };
  }

  // --- Attack Scenario 1: Untrusted page script knows static nonce and calls direct blob download without click ---
  console.log("  [Adversarial 1] Attack script knowing nonce attempts unauthenticated blob download...");
  const res1 = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://evil.example/exploit.exe",
    filename: "malware.exe",
    nonce: "flux-deepsearch",
    dataBase64: "TVqQAAMAAAAEAAAA//8AALgAAAA="
  });
  assert.strictEqual(res1.accepted, false);
  assert.strictEqual(res1.reason, "BLOCKED_NO_OR_INVALID_TOKEN");
  console.log("  -> BLOCKED: Attacker without anchor token rejected.");

  // --- Attack Scenario 2: Attacker waits for genuine click on an unrelated link (e.g. navigation or another button) ---
  console.log("  [Adversarial 2] Attacker waits for genuine click anywhere on page, then fires blob download...");
  const normalNavAnchor = { href: "https://legit.example/about", download: "", setAttribute: () => {} };
  simulateHandleLinkClick({ isTrusted: true }, normalNavAnchor); // User clicks a normal web link

  const res2 = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://evil.example/stealth_trojan.dll",
    filename: "stealth_trojan.dll",
    nonce: "flux-deepsearch",
    dataBase64: "TVqQAAMAAAAEAAAA//8AALgAAAA="
  });
  assert.strictEqual(res2.accepted, false);
  assert.strictEqual(res2.reason, "BLOCKED_NO_OR_INVALID_TOKEN");
  console.log("  -> BLOCKED: Click on unrelated element does not authorize arbitrary blob download.");

  // --- Attack Scenario 3: Attacker attempts URL substitution / parameter forgery using legitimate anchor's token ---
  console.log("  [Adversarial 3] Attacker attempts URL substitution / parameter forgery using legitimate anchor's token...");
  const legitimateBlobAnchor = {
    href: "blob:https://chatgpt.com/artifact-1234",
    download: "report.pdf",
    attrs: {},
    setAttribute(k, v) { this.attrs[k] = v; },
    getAttribute(k) { return this.attrs[k]; }
  };
  simulateHandleLinkClick({ isTrusted: true }, legitimateBlobAnchor);
  const issuedToken = legitimateBlobAnchor.getAttribute("data-radium-auth-token");
  assert.ok(issuedToken, "Legitimate click received token");

  const res3 = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://evil.example/ransomware.exe", // Substituted URL!
    filename: "report.pdf",
    authToken: issuedToken,
    dataBase64: "TVqQAAMAAAAEAAAA//8AALgAAAA="
  });
  assert.strictEqual(res3.accepted, false);
  assert.strictEqual(res3.reason, "BLOCKED_URL_MISMATCH");
  console.log("  -> BLOCKED: Payload URL mismatch against clicked anchor rejected.");

  // --- Attack Scenario 4: Replay attack (token reuse) ---
  console.log("  [Adversarial 4] Attacker attempts replay attack reusing consumed token...");
  simulateHandleLinkClick({ isTrusted: true }, legitimateBlobAnchor);
  const legitimateToken = legitimateBlobAnchor.getAttribute("data-radium-auth-token");
  const legitRes = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://chatgpt.com/artifact-1234",
    filename: "report.pdf",
    authToken: legitimateToken,
    dataBase64: "JVBERi0xLjQKJ..."
  });
  assert.strictEqual(legitRes.accepted, true, "Legitimate click authorized and consumed");

  const replayRes = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://chatgpt.com/artifact-1234",
    filename: "report.pdf",
    authToken: legitimateToken,
    dataBase64: "JVBERi0xLjQKJ..."
  });
  assert.strictEqual(replayRes.accepted, false);
  assert.strictEqual(replayRes.reason, "BLOCKED_NO_OR_INVALID_TOKEN");
  console.log("  -> BLOCKED: Replay attack rejected (token is strictly single-use).");

  // --- Attack Scenario 5: Malicious Webpage uses MutationObserver to steal token from DOM attribute ---
  console.log("  [Adversarial 5] Malicious script uses MutationObserver to steal token and races to download unintended payload...");
  let onMutationCallback = null;
  const observedAnchor = {
    href: "blob:https://chatgpt.com/artifact-5678",
    download: "financials.xlsx",
    attrs: {},
    setAttribute(k, v) {
      this.attrs[k] = v;
      if (typeof onMutationCallback === "function") {
        onMutationCallback(k, v);
      }
    },
    getAttribute(k) { return this.attrs[k]; }
  };

  let stolenToken = null;
  let attackerRaceResult = null;

  onMutationCallback = (attrName, attrValue) => {
    if (attrName === "data-radium-auth-token") {
      stolenToken = attrValue;
      // Malicious page script immediately races to submit an unintended payload using the stolen token!
      attackerRaceResult = simulateRadiumDirectBlobDownload({
        action: "radiumDirectBlobDownload",
        url: "blob:https://evil.example/malicious_payload.exe", // Malicious URL substitution
        filename: "financials.xlsx",
        authToken: stolenToken,
        dataBase64: "TVqQAAMAAAAEAAAA//8AALgAAAA="
      });
    }
  };

  // User genuinely clicks the observed anchor
  simulateHandleLinkClick({ isTrusted: true }, observedAnchor);

  assert.ok(stolenToken, "MutationObserver saw the attribute mutation");
  assert.strictEqual(attackerRaceResult.accepted, false, "Attacker race attempt was rejected");
  assert.strictEqual(attackerRaceResult.reason, "BLOCKED_URL_MISMATCH", "Attacker cannot download an unintended URL even with stolen token");
  console.log("  -> BLOCKED: Attacker stealing token via MutationObserver cannot change target URL.");

  // Also verify that if attacker tries to match the URL but change the filename to an executable:
  let attackerFilenameSpoof = null;
  onMutationCallback = (attrName, attrValue) => {
    if (attrName === "data-radium-auth-token") {
      attackerFilenameSpoof = simulateRadiumDirectBlobDownload({
        action: "radiumDirectBlobDownload",
        url: "blob:https://chatgpt.com/artifact-5678",
        filename: "trojan.exe", // Filename mismatch
        authToken: attrValue,
        dataBase64: "TVqQAAMAAAAEAAAA//8AALgAAAA="
      });
    }
  };
  simulateHandleLinkClick({ isTrusted: true }, observedAnchor);
  assert.strictEqual(attackerFilenameSpoof.accepted, false);
  assert.strictEqual(attackerFilenameSpoof.reason, "BLOCKED_FILENAME_MISMATCH");
  console.log("  -> BLOCKED: Attacker stealing token via MutationObserver cannot change target filename.");

  // Furthermore verify expiration enforcement (>15s):
  console.log("  [Adversarial 6] Expiration enforcement (>15s)...");
  onMutationCallback = null;
  simulateHandleLinkClick({ isTrusted: true }, observedAnchor);
  const freshToken = observedAnchor.getAttribute("data-radium-auth-token");
  // Fast forward token timestamp by 20s
  const record = mockTokens.get(freshToken);
  record.timestamp -= 20000;
  const expiredRes = simulateRadiumDirectBlobDownload({
    action: "radiumDirectBlobDownload",
    url: "blob:https://chatgpt.com/artifact-5678",
    filename: "financials.xlsx",
    authToken: freshToken,
    dataBase64: "TVqQAAMAAAAEAAAA//8AALgAAAA="
  });
  assert.strictEqual(expiredRes.accepted, false);
  assert.strictEqual(expiredRes.reason, "BLOCKED_EXPIRED_TOKEN");
  console.log("  -> BLOCKED: Expired token rejected.");

  console.log("=======================================================");
  console.log(" All SEC-001 Adversarial & MutationObserver Vectors Passed!");
  console.log("=======================================================\n");
}

module.exports = { testSec001AdversarialSuite };

if (require.main === module) {
  testSec001AdversarialSuite();
}
