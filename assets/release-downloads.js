(() => {
  "use strict";

  const repository = "dmikey/cara";
  const releases = `https://github.com/${repository}/releases`;
  const checksums = "SHA256SUMS.txt";
  // Each platform can ship independently. Require the release publisher's
  // corresponding signing/build receipt as well as a digest and checksum file.
  const packages = {
    mac: [
      ["Cara-macOS-arm64-JIT-signed.zip", "MAC-SIGNING.json"],
      ["Cara-macOS-arm64-JIT-signed.zip", "Cara-macOS-arm64-JIT-signed.zip.json"],
    ],
    android: [
      ["Cara-Android-arm64-GLES-signed.apk", "ANDROID-SIGNING.json"],
      ["Cara-Android-arm64-signed.apk", "android-signed.json"],
    ],
    linux: [["Cara-Linux-x86_64-OpenGL.tar.gz", "linux-build.json"]],
    windows: [["Cara-Windows-x86_64-OpenGL.zip", "windows-build.json"]],
  };
  const tagPattern = /^Cara\.RC\.\d{8}(?!00)\d{2}$/;

  function candidate(release) {
    if (!release || release.draft !== false ||
        !tagPattern.test(release.tag_name) || !Array.isArray(release.assets)) {
      return null;
    }
    const assets = new Set(release.assets.filter((asset) =>
      asset && asset.state === "uploaded" && asset.size > 0 &&
      /^sha256:[a-f0-9]{64}$/.test(asset.digest)
    ).map((asset) => asset.name));
    return assets.has(checksums) ? { tag: release.tag_name, assets } : null;
  }

  function updateDownloads(candidates) {
    for (const [platform, choices] of Object.entries(packages)) {
      const notes = document.getElementById(`candidate-${platform}-notes`);
      // Never regress below the checked-in, public fallback.
      const fallback = notes.textContent.trim();
      const minimum = tagPattern.test(fallback) ? fallback : "";
      for (const release of candidates) {
        if (release.tag < minimum) break;
        const choice = choices.find(([asset, receipt]) =>
          release.assets.has(asset) && release.assets.has(receipt));
        if (!choice) continue;
        const base = `${releases}/download/${release.tag}`;
        document.getElementById(`candidate-${platform}`).href = `${base}/${choice[0]}`;
        document.getElementById(`candidate-${platform}-checksum`).href = `${base}/${checksums}`;
        notes.href = `${releases}/tag/${release.tag}`;
        notes.textContent = release.tag;
        document.getElementById(`candidate-${platform}-row`).hidden = false;
        break;
      }
    }
  }

  fetch(`https://api.github.com/repos/${repository}/releases?per_page=100`, {
    credentials: "omit",
  }).then((response) => response.ok ? response.json() : []).then((items) => {
    if (!Array.isArray(items)) return;
    const candidates = items.map(candidate).filter(Boolean);
    candidates.sort((a, b) => b.tag.localeCompare(a.tag));
    updateDownloads(candidates);
  }).catch(() => {
    // Preserve verified static links if GitHub is unavailable or rate-limited.
  });
})();
