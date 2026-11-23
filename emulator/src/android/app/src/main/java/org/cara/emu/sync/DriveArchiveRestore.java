// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.sync;

import android.util.AtomicFile;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;

/** Replaces a complete archive under the emulator's filesystem lock. */
final class DriveArchiveRestore {
    private final File root;
    private final AtomicFile journal;

    DriveArchiveRestore(File root) {
        this.root = root;
        journal = new AtomicFile(new File(root, ".cara-drive-restore"));
    }

    void recover() throws IOException {
        String path;
        try (DataInputStream in = new DataInputStream(journal.openRead())) {
            path = in.readUTF();
            if (in.read() != -1) {
                throw new IOException("Invalid save restore journal");
            }
        } catch (FileNotFoundException missing) {
            if (journal.getBaseFile().exists()) {
                throw missing;
            }
            return;
        }
        File target = unit(path);
        File staged = sibling(target, "staged");
        File previous = sibling(target, "previous");
        if (previous.exists() && !target.exists()) {
            rename(previous, target);
        }
        removeTree(staged);
        removeTree(previous);
        journal.delete();
    }

    void replace(String path, File snapshot) throws IOException {
        File target = unit(path);
        File directory = target.getParentFile();
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Could not create the save directory");
        }
        File staged = sibling(target, "staged");
        File previous = sibling(target, "previous");
        if (staged.exists() || previous.exists()) {
            throw new IOException("A prior save restore requires recovery");
        }

        FileOutputStream journalStream = journal.startWrite();
        try {
            DataOutputStream out = new DataOutputStream(journalStream);
            out.writeUTF(path);
            out.flush();
            journal.finishWrite(journalStream);
        } catch (IOException failure) {
            journal.failWrite(journalStream);
            throw failure;
        }

        // AtomicFile can report a failed rename only through logging. Verify that the
        // committed journal is readable before touching the original save.
        try (DataInputStream in = new DataInputStream(journal.openRead())) {
            if (!path.equals(in.readUTF()) || in.read() != -1) {
                throw new IOException("Could not commit the save restore journal");
            }
        }

        try {
            try (FileInputStream in = new FileInputStream(snapshot)) {
                DriveSaveArchive.unpack(in, staged);
            }
            if (target.exists()) {
                rename(target, previous);
            }
            rename(staged, target);
        } catch (IOException failure) {
            try {
                recover();
            } catch (IOException recoveryFailure) {
                failure.addSuppressed(recoveryFailure);
            }
            throw failure;
        }
        recover();
    }

    private File unit(String path) throws IOException {
        if (!DriveSaveArchive.isUnit(path)) {
            throw new IOException("Invalid save unit");
        }
        File file = new File(root, path);
        if (!file.equals(file.getCanonicalFile())) {
            throw new IOException("Invalid save directory");
        }
        return file;
    }

    private static File sibling(File target, String suffix) throws IOException {
        File file = new File(target.getParentFile(), ".cara-drive-" + target.getName() + "-" + suffix);
        if (!file.equals(file.getCanonicalFile())) {
            throw new IOException("Invalid save restore path");
        }
        return file;
    }

    private static void rename(File source, File destination) throws IOException {
        if (!source.renameTo(destination)) {
            throw new IOException("Could not move the save archive");
        }
    }

    private static void removeTree(File file) throws IOException {
        if (!file.getCanonicalFile().equals(file)) {
            throw new IOException("Invalid path during save recovery");
        }
        if (!file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            File[] entries = file.listFiles();
            if (entries == null) {
                throw new IOException("Could not read the previous save archive");
            }
            for (File entry : entries) {
                removeTree(entry);
            }
        }
        if (!file.delete()) {
            throw new IOException("Could not remove the previous save archive");
        }
    }
}
