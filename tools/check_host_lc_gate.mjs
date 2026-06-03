#!/usr/bin/env bun

import { readFileSync } from "node:fs";
import { basename } from "node:path";

function usage() {
  console.error(`Usage: bun tools/check_host_lc_gate.mjs LOG [--min-cycles N] [--expect-productinfo N]\n\nChecks a host LC harness log for the current regression markers.`);
}

const args = process.argv.slice(2);
if (args.length < 1 || args.includes("--help") || args.includes("-h")) {
  usage();
  process.exit(args.length < 1 ? 2 : 0);
}

const logPath = args[0];
let minCycles = 0;
let expectProductInfo = null;
for (let i = 1; i < args.length; i++) {
  const arg = args[i];
  if (arg === "--min-cycles" && i + 1 < args.length) {
    minCycles = Number(args[++i]);
  } else if (arg === "--expect-productinfo" && i + 1 < args.length) {
    expectProductInfo = Number(args[++i]);
  } else {
    console.error(`Unknown or incomplete argument: ${arg}`);
    usage();
    process.exit(2);
  }
}

const text = readFileSync(logPath, "utf8");
const failures = [];

function countLiteral(needle) {
  let count = 0;
  let index = 0;
  while ((index = text.indexOf(needle, index)) !== -1) {
    count++;
    index += needle.length;
  }
  return count;
}

function requireIncludes(needle, label = needle) {
  if (!text.includes(needle)) {
    failures.push(`missing ${label}`);
  }
}

const forbidden = [
  "A-trap table synthetic read",
  "0x00007fba",
  "pc=0xffffffff",
  "addr=0x01000000",
  "0xffffffd8",
  "0x001fdfec",
  "LC diagnostic exception stack",
  "LC illegal instruction callback",
  "LC post-reset invalid execution trace",
];

requireIncludes("HOST_LC_OK");
requireIncludes("LC ROM validation: partition_size_ok=yes first_long_ok=yes", "ROM validation success");

if (expectProductInfo !== null) {
  requireIncludes(`productinfo_default_rsrcs=${expectProductInfo}`, `productinfo_default_rsrcs=${expectProductInfo}`);
}

for (const marker of forbidden) {
  const count = countLiteral(marker);
  if (count !== 0) {
    failures.push(`forbidden marker '${marker}' count=${count}`);
  }
}

const resultMatches = [...text.matchAll(/LC ROM entry micro-probe result: cycles=(\d+) .*?stopped_on_zero_ram=(\d+) stopped_on_monitor=(\d+)/g)];
let result = null;
if (resultMatches.length === 0) {
  failures.push("missing LC ROM entry micro-probe result");
} else {
  const last = resultMatches[resultMatches.length - 1];
  result = {
    cycles: Number(last[1]),
    stoppedOnZeroRam: Number(last[2]),
    stoppedOnMonitor: Number(last[3]),
  };
  if (result.cycles < minCycles) {
    failures.push(`cycles ${result.cycles} < min-cycles ${minCycles}`);
  }
  if (result.stoppedOnZeroRam !== 0) {
    failures.push(`stopped_on_zero_ram=${result.stoppedOnZeroRam}`);
  }
  if (result.stoppedOnMonitor !== 0) {
    failures.push(`stopped_on_monitor=${result.stoppedOnMonitor}`);
  }
}

const summary = {
  log: basename(logPath),
  ok: failures.length === 0,
  result,
  forbiddenCounts: Object.fromEntries(forbidden.map((marker) => [marker, countLiteral(marker)])),
};

console.log(JSON.stringify(summary, null, 2));

if (failures.length !== 0) {
  console.error("HOST_LC_GATE_FAIL");
  for (const failure of failures) {
    console.error(`- ${failure}`);
  }
  process.exit(1);
}

console.log("HOST_LC_GATE_OK");
