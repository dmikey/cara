// SPDX-License-Identifier: GPL-2.0-or-later
package org.cara.emu.sync;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.Comparator;
import java.util.regex.Pattern;

/** A deterministic snapshot of one game save directory, including its format metadata. */
final class DriveSaveArchive {
    static final long LIMIT = 128L * 1024 * 1024;
    private static final int MAGIC = 0x43415331; // CAS1
    private static final int END = 0;
    private static final int DIRECTORY = 1;
    private static final int FILE = 2;
    private static final int MAX_ENTRIES = 32768;

    private static final Pattern UNIT = Pattern.compile(
            "sdmc/Nintendo 3DS/[0-9a-fA-F]{32}/[0-9a-fA-F]{32}/"
            + "(?:title/[0-9a-fA-F]{8}/[0-9a-fA-F]{8}/data|extdata/[0-9a-fA-F]{8}/[0-9a-fA-F]{8})"
            + "|nand/data/[0-9a-fA-F]{32}/extdata/[0-9a-fA-F]{8}/[0-9a-fA-F]{8}");
    private static final Pattern EXCLUDED = Pattern.compile(
            ".*\\.(?:3ds|z3ds|zcci|cci|cxi|cia|app|3dsx|nds|elf|axf|deltaskin)$",
            Pattern.CASE_INSENSITIVE);

    private DriveSaveArchive() {}

    static boolean isUnit(String path) {
        return UNIT.matcher(path).matches();
    }

    static void write(File directory, OutputStream destination) throws IOException {
        if (!directory.isDirectory()) {
            throw new IOException("Save directory is unavailable");
        }
        DataOutputStream out = new DataOutputStream(destination);
        Budget budget = new Budget();
        budget.addBytes(4);
        out.writeInt(MAGIC);
        writeDirectory(directory, "", out, budget, new byte[64 * 1024]);
        budget.addBytes(1);
        out.writeByte(END);
        out.flush();
    }

    private static void writeDirectory(File directory, String prefix, DataOutputStream out,
                                       Budget budget, byte[] buffer) throws IOException {
        File[] children = directory.listFiles();
        if (children == null) {
            throw new IOException("Could not read a save folder");
        }
        Arrays.sort(children, Comparator.comparing(File::getName));
        for (File child : children) {
            String relative = prefix + child.getName();
            validateEntry(relative);
            if (!child.getCanonicalFile().equals(child)) {
                throw new IOException("Save archive contains a link or alias");
            }
            budget.addEntry();
            // writeUTF writes two length bytes and at most three bytes per UTF-16 code unit.
            budget.addBytes(3L + 3L * relative.length());
            if (child.isDirectory()) {
                out.writeByte(DIRECTORY);
                out.writeUTF(relative);
                writeDirectory(child, relative + "/", out, budget, buffer);
            } else if (child.isFile() && !EXCLUDED.matcher(relative).matches()) {
                long size = child.length();
                budget.addBytes(8 + size);
                out.writeByte(FILE);
                out.writeUTF(relative);
                out.writeLong(size);
                try (InputStream in = new FileInputStream(child)) {
                    copy(in, out, size, buffer);
                    if (in.read() != -1) {
                        throw new IOException("Save changed while creating its snapshot");
                    }
                }
            } else {
                throw new IOException("Save archive contains an unsupported file");
            }
        }
    }

    static void unpack(InputStream source, File directory) throws IOException {
        if (!directory.mkdir()) {
            throw new IOException("Could not create a staging directory");
        }
        DataInputStream in = new DataInputStream(source);
        if (in.readInt() != MAGIC) {
            throw new IOException("Invalid Cara save archive");
        }
        byte[] buffer = new byte[64 * 1024];
        Budget budget = new Budget();
        budget.addBytes(4);
        for (int type = in.readUnsignedByte(); type != END; type = in.readUnsignedByte()) {
            if (type != DIRECTORY && type != FILE) {
                throw new IOException("Invalid save archive entry");
            }
            String relative = in.readUTF();
            validateEntry(relative);
            budget.addEntry();
            budget.addBytes(3L + 3L * relative.length());
            File entry = new File(directory, relative);
            if (entry.exists() || !entry.getCanonicalFile().equals(entry) ||
                    !entry.getParentFile().isDirectory()) {
                throw new IOException("Invalid destination in save archive");
            }
            if (type == DIRECTORY) {
                if (!entry.mkdir()) {
                    throw new IOException("Could not create a save folder");
                }
            } else {
                if (EXCLUDED.matcher(relative).matches()) {
                    throw new IOException("Unsupported file in save archive");
                }
                long size = in.readLong();
                if (size < 0 || size > LIMIT) {
                    throw new IOException("Invalid size in save archive");
                }
                budget.addBytes(8 + size);
                try (FileOutputStream out = new FileOutputStream(entry)) {
                    copy(in, out, size, buffer);
                    out.getFD().sync();
                }
            }
        }
        budget.addBytes(1);
        if (in.read() != -1) {
            throw new IOException("Unexpected trailing data in save archive");
        }
    }

    private static void validateEntry(String relative) throws IOException {
        if (relative.length() > 2048 || relative.indexOf('\\') >= 0) {
            throw new IOException("Invalid save archive path");
        }
        int components = 0;
        for (String part : relative.split("/", -1)) {
            if (++components > 64 || part.isEmpty() || part.equals(".") || part.equals("..")) {
                throw new IOException("Invalid save archive path");
            }
            for (int i = 0; i < part.length(); i++) {
                if (Character.isISOControl(part.charAt(i))) {
                    throw new IOException("Invalid save archive path");
                }
            }
        }
    }

    private static void copy(InputStream in, OutputStream out, long size, byte[] buffer)
            throws IOException {
        while (size > 0) {
            int count = in.read(buffer, 0, (int)Math.min(size, buffer.length));
            if (count <= 0) {
                throw new IOException("Incomplete save archive");
            }
            out.write(buffer, 0, count);
            size -= count;
        }
    }

    private static final class Budget {
        private long remaining = LIMIT;
        private int entries;

        void addBytes(long amount) throws IOException {
            if (amount > remaining) {
                throw new IOException("Save archive exceeds the supported size");
            }
            remaining -= amount;
        }

        void addEntry() throws IOException {
            if (++entries > MAX_ENTRIES) {
                throw new IOException("Save archive contains too many files");
            }
        }
    }
}
