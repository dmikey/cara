// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.sync;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.UUID;

final class DriveStorage {
    static final String SCOPE = "https://www.googleapis.com/auth/drive.appdata";
    private static final String HOST = "https://www.googleapis.com";
    private static final String SAVE_NAME = "cara-android-save-v2-saves";
    private static final String STATE_NAME = "cara-android-save-v1-states";
    private static final String UPLOAD = "/upload/drive/v3/files?uploadType=";
    private static final String FILE_FIELDS = "&fields=id%2Cdescription%2Csize%2CcreatedTime";
    private static final int MULTIPART_LIMIT = 5 * 1024 * 1024;
    private static final int JSON_LIMIT = 8 * 1024 * 1024;
    private final String token;

    static final class Version {
        final String id;
        final String path;
        final String digest;
        final String createdTime;
        final long size;
        final List<String> parents = new ArrayList<>();

        Version(JSONObject file) throws JSONException, IOException {
            JSONObject description = new JSONObject(file.getString("description"));
            id = file.getString("id");
            path = description.getString("path");
            digest = description.getString("sha256");
            size = file.getLong("size");
            createdTime = file.getString("createdTime");
            JSONArray array = description.getJSONArray("parents");
            for (int i = 0; i < array.length(); i++) {
                parents.add(array.getString(i));
            }
            if (DriveSaveSync.limit(path) < size || size < 0 || !digest.matches("[0-9a-f]{64}")) {
                throw new IOException("Invalid save metadata");
            }
        }
    }

    DriveStorage(String token) {
        this.token = token;
    }

    String account() throws IOException, JSONException {
        return get("/drive/v3/about?fields=user%28permissionId%29")
                .getJSONObject("user").getString("permissionId");
    }

    List<Version> versions(int contents) throws IOException, JSONException {
        List<Version> versions = new ArrayList<>();
        String saves = "name = '" + SAVE_NAME + "'";
        String states = "name = '" + STATE_NAME + "'";
        String query = contents == (DriveSaveSync.GAME_SAVES | DriveSaveSync.SAVE_STATES)
                ? "(" + saves + " or " + states + ")"
                : (contents == DriveSaveSync.SAVE_STATES ? states : saves);
        String next = "";
        do {
            String url = "/drive/v3/files?spaces=appDataFolder&pageSize=100"
                    + "&fields=nextPageToken%2Cfiles%28id%2Cdescription%2Csize%2CcreatedTime%29"
                    + "&q=" + encode(query + " and trashed = false");
            JSONObject response = get(next.isEmpty() ? url : url + "&pageToken=" + encode(next));
            JSONArray files = response.optJSONArray("files");
            if (files != null) {
                for (int i = 0; i < files.length(); i++) {
                    versions.add(new Version(files.getJSONObject(i)));
                }
            }
            next = response.optString("nextPageToken");
        } while (!next.isEmpty());
        return versions;
    }

    Version upload(String path, String digest, File file, Collection<String> parents)
            throws IOException, JSONException {
        JSONObject description = new JSONObject().put("path", path).put("sha256", digest)
                .put("parents", new JSONArray(parents));
        String name = path.startsWith("states/") ? STATE_NAME : SAVE_NAME;
        JSONObject metadata = new JSONObject().put("name", name)
                .put("parents", new JSONArray().put("appDataFolder"))
                .put("description", description.toString());
        if (file.length() > MULTIPART_LIMIT) {
            return uploadLarge(file, metadata.toString().getBytes(StandardCharsets.UTF_8));
        }
        String boundary = "cara-" + UUID.randomUUID();
        byte[] before = ("--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"
                + metadata + "\r\n--" + boundary + "\r\nContent-Type: application/octet-stream\r\n\r\n")
                .getBytes(StandardCharsets.UTF_8);
        byte[] after = ("\r\n--" + boundary + "--\r\n").getBytes(StandardCharsets.US_ASCII);
        HttpURLConnection request = open(UPLOAD + "multipart" + FILE_FIELDS);
        try {
            request.setRequestMethod("POST");
            request.setDoOutput(true);
            request.setRequestProperty("Content-Type", "multipart/related; boundary=" + boundary);
            request.setFixedLengthStreamingMode(before.length + file.length() + after.length);
            try (OutputStream out = request.getOutputStream();
                 InputStream in = new FileInputStream(file)) {
                out.write(before);
                copy(in, out, file.length());
                out.write(after);
            }
            return new Version(readJson(request));
        } finally {
            request.disconnect();
        }
    }

    private Version uploadLarge(File file, byte[] metadata) throws IOException, JSONException {
        HttpURLConnection begin = open(UPLOAD + "resumable" + FILE_FIELDS);
        String location;
        try {
            begin.setRequestMethod("POST");
            begin.setDoOutput(true);
            begin.setRequestProperty("Content-Type", "application/json; charset=UTF-8");
            begin.setRequestProperty("X-Upload-Content-Type", "application/octet-stream");
            begin.setRequestProperty("X-Upload-Content-Length", Long.toString(file.length()));
            begin.setFixedLengthStreamingMode(metadata.length);
            try (OutputStream out = begin.getOutputStream()) {
                out.write(metadata);
            }
            try (InputStream ignored = response(begin)) {
                location = begin.getHeaderField("Location");
            }
        } finally {
            begin.disconnect();
        }
        if (location == null || !location.startsWith(HOST + "/upload/drive/v3/files?")) {
            throw new IOException("Invalid Drive upload session");
        }
        HttpURLConnection request = open(location.substring(HOST.length()));
        try {
            request.setRequestMethod("PUT");
            request.setDoOutput(true);
            request.setRequestProperty("Content-Type", "application/octet-stream");
            request.setFixedLengthStreamingMode(file.length());
            try (OutputStream out = request.getOutputStream();
                 InputStream in = new FileInputStream(file)) {
                copy(in, out, file.length());
            }
            return new Version(readJson(request));
        } finally {
            request.disconnect();
        }
    }

    void download(Version version, OutputStream out) throws IOException {
        HttpURLConnection request = open("/drive/v3/files/" + encode(version.id) + "?alt=media");
        try {
            try (InputStream in = response(request)) {
                if (copy(in, out, version.size) != version.size) {
                    throw new IOException("Incomplete cloud save");
                }
            }
        } finally {
            request.disconnect();
        }
    }

    static long copy(InputStream in, OutputStream out, long limit) throws IOException {
        long copied = 0;
        byte[] buffer = new byte[64 * 1024];
        for (int size; (size = in.read(buffer)) != -1;) {
            if (size > limit - copied) {
                throw new IOException("Save exceeds its declared size");
            }
            out.write(buffer, 0, size);
            copied += size;
        }
        return copied;
    }

    private JSONObject get(String path) throws IOException, JSONException {
        HttpURLConnection request = open(path);
        try {
            return readJson(request);
        } finally {
            request.disconnect();
        }
    }

    private static JSONObject readJson(HttpURLConnection request) throws IOException, JSONException {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        try (InputStream in = response(request)) {
            copy(in, buffer, JSON_LIMIT);
        }
        return new JSONObject(new String(buffer.toByteArray(), StandardCharsets.UTF_8));
    }

    private static InputStream response(HttpURLConnection request) throws IOException {
        int status = request.getResponseCode();
        if (status < 200 || status >= 300) {
            throw new IOException("Google Drive returned HTTP " + status);
        }
        return request.getInputStream();
    }

    private HttpURLConnection open(String path) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(HOST + path).openConnection();
        connection.setInstanceFollowRedirects(false);
        connection.setRequestProperty("Authorization", "Bearer " + token);
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(45000);
        return connection;
    }

    private static String encode(String value) throws IOException {
        return URLEncoder.encode(value, "UTF-8");
    }
}
