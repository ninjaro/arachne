import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const distDirectory = join(dirname(scriptDirectory), "dist");
const views = ["browse", "recommendations", "evolution", "islands", "research"];

async function json(relative) {
  return JSON.parse(await readFile(join(distDirectory, relative), "utf8"));
}

const index = await json("api/v1/index.json");
assert.equal(index.kind, "arachne.viewer.index");
assert.ok(index.datasets.catalog.count >= 0);

for (const view of views) {
  const descriptor = await json(`api/v1/views/${view}.json`);
  assert.equal(descriptor.name, view);
  assert.equal(descriptor.kind, "arachne.viewer.view");
  await access(join(distDirectory, view, "index.html"));
}

const browse = await json("api/v1/views/browse.json");
assert.ok(browse.staticPagination.pageCount >= 1);
const firstPage = await json("api/v1/browse/pages/1.json");
assert.equal(firstPage.page, 1);
assert.equal(firstPage.pageSize, browse.staticPagination.pageSize);
assert.ok(Array.isArray(firstPage.items));

const rootHtml = await readFile(join(distDirectory, "index.html"), "utf8");
assert.match(rootHtml, /Canonical JSON:/);
assert.match(rootHtml, /api\/v1\/index\.json/);
assert.match(rootHtml, /rel="canonical"/);
assert.match(rootHtml, /rel="alternate"/);
assert.match(rootHtml, /rel="service-desc"/);
assert.doesNotMatch(rootHtml, /arachne-document-links/);

console.log("static API checks passed");
