// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.sync;

import android.content.Context;
import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.system.Os;

import org.cara.emu.utils.CaraDirectory;
import org.json.JSONException;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.DigestOutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.locks.ReentrantLock;
import java.util.regex.Pattern;

public final class DriveSaveSync {
    private static final ReentrantLock LOCAL_IO = new ReentrantLock(true);
    private static final long STATE_LIMIT = 768L * 1024 * 1024 + 256;
    private static final Pattern STATE = Pattern.compile(
            "states/(?:[0-9A-F]{16}|homebrew-[0-9A-F]{64})\\.(?:0[1-9]|10)\\.carastate");
    static final int GAME_SAVES = 1;
    static final int SAVE_STATES = 2;

    enum Choice { NONE, DEVICE, DRIVE }

    static boolean selected(String path, int contents) {
        return limit(path) >= 0 &&
                (contents & (path.startsWith("states/") ? SAVE_STATES : GAME_SAVES)) != 0;
    }

    static final class Result {
        int uploaded;
        int restored;
        int conflicts;
    }

    static class Busy extends IOException {
        private static final long serialVersionUID = 1L;

        Busy() {
            super("Close the running game before syncing saves");
        }
    }

    private final Context context;
    private final DriveStorage drive;
    private final File root;
    private final DriveArchiveRestore archives;

    DriveSaveSync(Context context, DriveStorage drive) throws IOException {
        this.context = context;
        this.drive = drive;
        root = new File(CaraDirectory.getUserDirectory()).getCanonicalFile();
        archives = new DriveArchiveRestore(root);
    }

    public static void runGame(Context context, Runnable game, Runnable failedRecovery) {
        LOCAL_IO.lock();
        try {
            try {
                File root = new File(CaraDirectory.getUserDirectory()).getCanonicalFile();
                new DriveArchiveRestore(root).recover();
            } catch (IOException error) {
                failedRecovery.run();
                return;
            }
            game.run();
        } finally {
            LOCAL_IO.unlock();
        }
        DriveSyncController.afterGame(context);
    }

    private static void lockLocal() throws Busy {
        if (!LOCAL_IO.tryLock()) {
            throw new Busy();
        }
    }

    static long limit(String path) {
        if (path.length() > 2048 || path.indexOf('\\') >= 0) {
            return -1;
        }
        for (String part : path.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) {
                return -1;
            }
            for (int i = 0; i < part.length(); i++) {
                if (Character.isISOControl(part.charAt(i))) {
                    return -1;
                }
            }
        }
        if (STATE.matcher(path).matches()) {
            return STATE_LIMIT;
        }
        return DriveSaveArchive.isUnit(path) ? DriveSaveArchive.LIMIT : -1;
    }

    Result sync(String account, int contents, Choice choice) throws IOException, JSONException {
        SharedPreferences baseline = context.getSharedPreferences("drive-baseline-" + account, 0);
        Map<String, List<DriveStorage.Version>> remote = new HashMap<>();
        for (DriveStorage.Version version : drive.versions(contents)) {
            if (selected(version.path, contents)) {
                remote.computeIfAbsent(version.path, key -> new ArrayList<>()).add(version);
            }
        }

        Set<String> paths = new TreeSet<>(remote.keySet());
        lockLocal();
        try {
            archives.recover();
            if ((contents & GAME_SAVES) != 0) {
                collect("sdmc/Nintendo 3DS", paths);
                collect("nand/data", paths);
            }
            if ((contents & SAVE_STATES) != 0) {
                collect("states", paths);
            }
        } finally {
            LOCAL_IO.unlock();
        }

        Result result = new Result();
        for (String path : paths) {
            File file = local(path);
            List<DriveStorage.Version> all = remote.get(path);
            List<DriveStorage.Version> heads = heads(all);
            DriveStorage.Version cloud = newest(heads);
            String base = baseline.getString(path, null);
            String device;
            lockLocal();
            try {
                device = fingerprint(path, file);
            } finally {
                LOCAL_IO.unlock();
            }

            if (device == null && base != null && choice != Choice.DRIVE) {
                // A deleted local slot stays deleted. Drive retains its last recoverable copy.
                continue;
            }
            boolean cloudAgrees = cloud == null ||
                    heads.stream().allMatch(version -> version.digest.equals(cloud.digest));
            if (cloudAgrees && cloud != null && cloud.digest.equals(device)) {
                baseline.edit().putString(path, device).apply();
                continue;
            }
            boolean upload = device != null && (cloud == null ||
                    (cloudAgrees && cloud.digest.equals(base)));
            boolean download = cloud != null && cloudAgrees && (device == null || device.equals(base));
            boolean resolving = !upload && !download;
            if (upload || (resolving && choice == Choice.DEVICE && device != null)) {
                upload(path, file, device, heads);
                baseline.edit().putString(path, device).apply();
                result.uploaded++;
            } else if (download || (resolving && choice == Choice.DRIVE && cloud != null)) {
                restore(file, device, cloud, !cloudAgrees ? heads : null, resolving);
                baseline.edit().putString(path, cloud.digest).apply();
                result.restored++;
            } else {
                result.conflicts++;
            }
        }
        return result;
    }

    private static List<DriveStorage.Version> heads(List<DriveStorage.Version> versions)
            throws IOException {
        List<DriveStorage.Version> heads = new ArrayList<>();
        if (versions == null) {
            return heads;
        }
        Set<String> ancestors = new HashSet<>();
        for (DriveStorage.Version version : versions) {
            ancestors.addAll(version.parents);
        }
        for (DriveStorage.Version version : versions) {
            if (!ancestors.contains(version.id)) {
                heads.add(version);
            }
        }
        if (heads.isEmpty()) {
            throw new IOException("Invalid cloud save history");
        }
        return heads;
    }

    private static DriveStorage.Version newest(List<DriveStorage.Version> heads) {
        DriveStorage.Version newest = null;
        for (DriveStorage.Version version : heads) {
            if (newest == null || version.createdTime.compareTo(newest.createdTime) > 0 ||
                    (version.createdTime.equals(newest.createdTime) &&
                            version.id.compareTo(newest.id) > 0)) {
                newest = version;
            }
        }
        return newest;
    }

    private static List<String> parentIds(List<DriveStorage.Version> versions) {
        List<String> ids = new ArrayList<>(versions.size());
        for (DriveStorage.Version version : versions) {
            ids.add(version.id);
        }
        return ids;
    }

    private void upload(String path, File file, String device, List<DriveStorage.Version> heads)
            throws IOException, JSONException {
        File snapshot = File.createTempFile("cara-save-", ".tmp", context.getCacheDir());
        try {
            lockLocal();
            try {
                if (!device.equals(snapshot(path, file, snapshot))) {
                    throw new Busy();
                }
            } finally {
                LOCAL_IO.unlock();
            }
            drive.upload(path, device, snapshot, parentIds(heads));
        } finally {
            snapshot.delete();
        }
    }

    private void restore(File file, String device, DriveStorage.Version cloud,
                         List<DriveStorage.Version> merge, boolean preserveLocal)
            throws IOException, JSONException {
        File snapshot = File.createTempFile("cara-save-", ".tmp", context.getCacheDir());
        try {
            try (FileOutputStream out = new FileOutputStream(snapshot)) {
                drive.download(cloud, out);
            }
            if (!cloud.digest.equals(digest(snapshot, limit(cloud.path)))) {
                throw new IOException("Cloud save checksum failed");
            }
            lockLocal();
            try {
                File target = local(cloud.path);
                archives.recover();
                if (!file.equals(target) || !Objects.equals(device,
                        fingerprint(cloud.path, target))) {
                    throw new Busy();
                }
                if (preserveLocal && device != null) {
                    File backup = local("drive-conflicts/" + device + "/" + cloud.path);
                    File parent = backup.getParentFile();
                    if (!parent.isDirectory() && !parent.mkdirs()) {
                        throw new IOException("Could not preserve the previous local save");
                    }
                    if (!backup.exists() || !device.equals(digest(backup, limit(cloud.path)))) {
                        File preserved = File.createTempFile(".cara-backup-", ".tmp", parent);
                        try {
                            if (!device.equals(snapshot(cloud.path, target, preserved))) {
                                throw new Busy();
                            }
                            rename(preserved, backup);
                        } finally {
                            preserved.delete();
                        }
                    }
                }
                if (DriveSaveArchive.isUnit(cloud.path)) {
                    archives.replace(cloud.path, snapshot);
                } else {
                    File directory = target.getParentFile();
                    if (!directory.isDirectory() && !directory.mkdirs()) {
                        throw new IOException("Could not create the save directory");
                    }
                    File temporary = File.createTempFile(".cara-sync-", ".tmp", directory);
                    try {
                        copy(snapshot, temporary, limit(cloud.path));
                        rename(temporary, target);
                    } finally {
                        temporary.delete();
                    }
                }
            } finally {
                LOCAL_IO.unlock();
            }
            if (merge != null) {
                drive.upload(cloud.path, cloud.digest, snapshot, parentIds(merge));
            }
        } finally {
            snapshot.delete();
        }
    }

    private void collect(String relative, Set<String> paths) throws IOException {
        File directory = local(relative);
        File[] children = directory.listFiles();
        if (children == null) {
            if (directory.exists()) {
                throw new IOException("Could not read the save directory");
            }
            return;
        }
        for (File child : children) {
            String path = relative + "/" + child.getName();
            if (!child.getCanonicalFile().equals(child)) {
                continue;
            }
            if (child.isDirectory()) {
                if (DriveSaveArchive.isUnit(path)) {
                    paths.add(path);
                } else if (archiveContainer(path)) {
                    collect(path, paths);
                }
            } else if (child.isFile() && STATE.matcher(path).matches()) {
                paths.add(path);
            }
        }
    }

    private static final Pattern ARCHIVE_CONTAINER = Pattern.compile(
            "sdmc/Nintendo 3DS/[0-9a-fA-F]{32}"
            + "(?:/[0-9a-fA-F]{32}(?:/(?:title(?:/[0-9a-fA-F]{8}){0,2}"
            + "|extdata(?:/[0-9a-fA-F]{8})?))?)?"
            + "|nand/data/[0-9a-fA-F]{32}(?:/extdata(?:/[0-9a-fA-F]{8})?)?");

    private static boolean archiveContainer(String path) {
        return ARCHIVE_CONTAINER.matcher(path).matches();
    }

    private static void rename(File source, File destination) throws IOException {
        try {
            Os.rename(source.getPath(), destination.getPath());
        } catch (ErrnoException error) {
            throw new IOException("Could not replace the local save", error);
        }
    }

    private static String fingerprint(String path, File source) throws IOException {
        if (!source.exists()) {
            return null;
        }
        if (DriveSaveArchive.isUnit(path)) {
            return snapshot(path, source, null);
        }
        if (!source.isFile()) {
            throw new IOException("Invalid local save state");
        }
        return digest(source, limit(path));
    }

    private static final OutputStream DISCARD = new OutputStream() {
        @Override public void write(int value) {}
        @Override public void write(byte[] bytes, int offset, int length) {}
    };

    private static String snapshot(String path, File source, File destination) throws IOException {
        if (!DriveSaveArchive.isUnit(path)) {
            return copy(source, destination, limit(path));
        }
        MessageDigest digest = sha256();
        try (FileOutputStream file = destination == null ? null : new FileOutputStream(destination)) {
            DriveSaveArchive.write(source, new DigestOutputStream(file == null ? DISCARD : file, digest));
            if (file != null) {
                file.getFD().sync();
            }
        }
        return hex(digest.digest());
    }

    private File local(String relative) throws IOException {
        File file = new File(root, relative);
        if (!file.equals(file.getCanonicalFile()) || !file.getPath().startsWith(root.getPath() + "/")) {
            throw new IOException("Invalid local save path");
        }
        return file;
    }

    static String accountKey(String account) {
        return hex(sha256().digest(account.getBytes(StandardCharsets.UTF_8)));
    }

    private static String digest(File file, long limit) throws IOException {
        return copy(file, null, limit);
    }

    private static String copy(File source, File destination, long limit) throws IOException {
        MessageDigest sha = sha256();
        try (InputStream in = new FileInputStream(source);
             FileOutputStream out = destination == null ? null : new FileOutputStream(destination)) {
            long remaining = limit;
            byte[] buffer = new byte[64 * 1024];
            for (int size; (size = in.read(buffer)) != -1;) {
                if (size > remaining) {
                    throw new IOException("Local save exceeds the supported size");
                }
                remaining -= size;
                sha.update(buffer, 0, size);
                if (out != null) {
                    out.write(buffer, 0, size);
                }
            }
            if (out != null) {
                out.getFD().sync();
            }
        }
        return hex(sha.digest());
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static String hex(byte[] digest) {
        StringBuilder value = new StringBuilder(digest.length * 2);
        for (byte b : digest) {
            value.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        }
        return value.toString();
    }
}
