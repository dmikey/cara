(() => {
  "use strict";

  const repository = "dmikey/cara";
  const releases = `https://github.com/${repository}/releases`;
  const mac = "Cara-macOS-arm64-JIT-signed.zip";
  const ios = "Cara-iOS-arm64-GLES3-unsigned.ipa";
  const android = "Cara-Android-arm64-signed.apk";
  const androidBundle = "Cara-Android-arm64-signed.aab";
  const desktop = {
    linux: "Cara-Linux-x86_64-OpenGL.tar.gz",
    windows: "Cara-Windows-x86_64-OpenGL.zip",
  };
  const checksums = "SHA256SUMS.txt";
  const initialCandidate = "Cara.RC.2026112301";

  function assetsFor(release) {
    if (release.draft !== false || release.prerelease !== true ||
        !/^Cara\.RC\.\d{8}(?!00)\d{2}$/.test(release.tag_name) ||
        !Array.isArray(release.assets)) {
      return null;
    }

    const assets = new Set(release.assets.filter((asset) =>
      asset.state === "uploaded" && asset.size > 0 &&
      /^sha256:[a-f0-9]{64}$/.test(asset.digest)
    ).map((asset) => asset.name));
    const required = release.tag_name === initialCandidate ?
      [mac, ios, checksums] : [
        mac, `${mac}.json`, ios, `${ios}.json`, android, androidBundle, "android-signed.json",
        desktop.linux, "linux-build.json", desktop.windows, "windows-build.json", checksums,
      ];
    if (!required.every((name) => assets.has(name))) {
      return null;
    }
    return { tag: release.tag_name, assets };
  }

  function updateDownloads(candidate) {
    const base = `${releases}/download/${candidate.tag}`;
    const notes = document.getElementById("candidate-notes");
    notes.href = `${releases}/tag/${candidate.tag}`;
    notes.textContent = `${candidate.tag} release notes`;
    document.getElementById("candidate-checksums").href = `${base}/${checksums}`;
    document.getElementById("candidate-mac").href = `${base}/${mac}`;
    document.getElementById("legacy-mac-checksum").hidden = candidate.tag !== initialCandidate;
    document.getElementById("candidate-ios").href = `${base}/${ios}`;

    for (const [platform, asset] of Object.entries(desktop)) {
      const row = document.getElementById(`candidate-${platform}-row`);
      const available = candidate.assets.has(asset) && candidate.assets.has(`${platform}-build.json`);
      if (available) {
        document.getElementById(`candidate-${platform}`).href = `${base}/${asset}`;
      }
      row.hidden = !available;
    }

    if (candidate.assets.has(android) && candidate.assets.has("android-signed.json")) {
      const download = document.getElementById("candidate-android");
      download.href = `${base}/${android}`;
      download.textContent = "Download signed APK";
      document.getElementById("android-download-note").textContent =
        "ARM64 release candidate APK. Updates to a Play install use the Play testing track.";
    }
  }

  fetch(`https://api.github.com/repos/${repository}/releases?per_page=100`, {
    credentials: "omit",
  }).then((response) => {
    if (!response.ok) {
      return [];
    }
    return response.json();
  }).then((items) => {
    if (!Array.isArray(items)) {
      return;
    }
    const candidates = items.map(assetsFor).filter(Boolean);
    candidates.sort((a, b) => b.tag.localeCompare(a.tag));
    if (candidates.length) {
      updateDownloads(candidates[0]);
    }
  }).catch(() => {
    // The page already contains verified links for offline and rate-limited visits.
  });
})();
