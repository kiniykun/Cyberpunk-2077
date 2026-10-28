// Local stand-in for the Vercel /game page (Plan B test): serves out/vercel-game/index.html at /game and /game/
// with COOP/COEP like vercel.json does. Everything else is 404 (all assets must come from the Worker).
//   node scripts/serve-vercel-game.mjs [--port 8099]
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import {OUT} from "./common.mjs";

const port = Number(process.argv[process.argv.indexOf("--port") + 1]) || 8099;
const file = path.join(OUT, "vercel-game", "index.html");
http.createServer((req, res) => {
  const p = new URL(req.url, "http://x").pathname;
  const h = {"Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin", "Cache-Control": "no-cache"};
  if (p === "/game/logo.png") { res.writeHead(200, {...h, "Content-Type": "image/png"}); return res.end(fs.readFileSync(path.join(OUT, "vercel-game", "logo.png"))); }
  if (p === "/game" || p === "/game/") {
    res.writeHead(200, {...h, "Content-Type": "text/html; charset=utf-8"});
    return res.end(req.method === "HEAD" ? undefined : fs.readFileSync(file));
  }
  res.writeHead(404, {...h, "Content-Type": "text/plain"}); res.end("not here (assets come from the Worker)");
  console.log("404", p);
}).listen(port, "127.0.0.1", () => console.log(`plan-b page on http://127.0.0.1:${port}/game`));
