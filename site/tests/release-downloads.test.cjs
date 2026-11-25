const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const js = fs.readFileSync(path.join(__dirname, "../assets/release-downloads.js"), "utf8");

const sha = "sha256:" + "a".repeat(64);
const asset = name => ({name, state:"uploaded", size:10, digest:sha});
const android = "Cara-Android-arm64-GLES-signed.apk";
const mac = "Cara-macOS-arm64-JIT-signed.zip";
const checksums = "SHA256SUMS.txt";
const release = (tag, names, overrides={}) => ({tag_name:tag, draft:false,
  prerelease:true, assets:names.map(asset), ...overrides});
const prefix = "https://github.com/dmikey/cara/releases/download/";

async function run(items, ok=true) {
  const nodes = new Map();
  for (const platform of ["mac","android","linux","windows"]) {
    for (const suffix of ["","-notes","-checksum","-row"]) {
      const id = "candidate-"+platform+suffix;
      nodes.set(id,{href:"fallback-"+id,textContent: platform==="android" ?
        "Cara.RC.2026112501" : platform==="mac" ? "Cara.RC.2026112301" : "Release",
        hidden:platform==="linux" || platform==="windows"});
    }
  }
  vm.runInNewContext(js,{
    document:{getElementById:id=>nodes.get(id)},
    fetch:async()=>({ok,json:async()=>items})
  });
  await new Promise(resolve=>setImmediate(resolve));
  return nodes;
}

(async()=>{
  const items = [
    release("Cara.RC.2026112601",[mac,"MAC-SIGNING.json",checksums],{draft:true}),
    release("Cara.RC.2026112602",[android,checksums]), // missing signing receipt
    release("Cara.RC.2026112603",[android,"ANDROID-SIGNING.json",checksums],{
      assets:[asset(android),asset(checksums),{...asset("ANDROID-SIGNING.json"),digest:null}]
    }),
    release("experimental-2026-11-26",[android,"ANDROID-SIGNING.json",checksums]),
    release("Cara.RC.2026112501",[android,"ANDROID-SIGNING.json",checksums]),
    release("Cara.RC.2026112502",[mac,"MAC-SIGNING.json",checksums]),
    release("Cara.RC.2026112401",[android,"ANDROID-SIGNING.json",checksums]),
  ];
  const nodes = await run(items);
  assert.equal(nodes.get("candidate-android").href,prefix+"Cara.RC.2026112501/"+android);
  assert.equal(nodes.get("candidate-mac").href,prefix+"Cara.RC.2026112502/"+mac);
  assert.equal(nodes.get("candidate-mac-checksum").href,prefix+"Cara.RC.2026112502/"+checksums);
  assert.equal(nodes.get("candidate-android-checksum").href,prefix+"Cara.RC.2026112501/"+checksums);
  assert.equal(nodes.get("candidate-linux-row").hidden,true);
  const newer = await run([release("Cara.RC.2026112701",["Cara-Android-arm64-signed.apk","android-signed.json",checksums]), ...items]);
  assert.equal(newer.get("candidate-android-notes").textContent,"Cara.RC.2026112701");
  const older = await run([items.at(-1)]);
  assert.equal(older.get("candidate-android").href,"fallback-candidate-android");
  const offline = await run(items,false);
  assert.equal(offline.get("candidate-android").href,"fallback-candidate-android");
  console.log("PASS independent releases, metadata, drafts, digests, future names, fallback and non-regression");
})().catch(error=>{console.error(error);process.exitCode=1});
