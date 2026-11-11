#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Radium Browser Extension Runtime Playwright Test Runner
const { runBrowserRuntimeTests } = require("./browser/test_extension_runtime.js");
runBrowserRuntimeTests().then((code) => process.exit(code));
