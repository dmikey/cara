// Modified for Cara on 2026-11-12; see NOTICE for attribution.
package org.cara.emu;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import androidx.annotation.Nullable;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.Collections;
import java.util.LinkedList;

public final class UserPathProvider extends DocumentsProvider {

    private final String[] DEFAULT_ROOT_PROJECTION = new String[] {
        DocumentsContract.Root.COLUMN_ROOT_ID,
        DocumentsContract.Root.COLUMN_MIME_TYPES,
        DocumentsContract.Root.COLUMN_FLAGS,
        DocumentsContract.Root.COLUMN_ICON,
        DocumentsContract.Root.COLUMN_TITLE,
        DocumentsContract.Root.COLUMN_SUMMARY,
        DocumentsContract.Root.COLUMN_DOCUMENT_ID,
        DocumentsContract.Root.COLUMN_AVAILABLE_BYTES
    };

    private final String[] DEFAULT_DOCUMENT_PROJECTION = new String[] {
        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
        DocumentsContract.Document.COLUMN_MIME_TYPE,
        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        DocumentsContract.Document.COLUMN_FLAGS,
        DocumentsContract.Document.COLUMN_SIZE
    };

    private String ROOT_ID;
    private File BASE_DIR;

    private File checkedFile(String documentId) throws FileNotFoundException {
        if (BASE_DIR == null) throw new FileNotFoundException("Cara storage is unavailable");
        if (documentId == null || !(documentId.equals(ROOT_ID) || documentId.startsWith(ROOT_ID + "/")))
            throw new FileNotFoundException("Document is not in Cara storage");
        try {
            File base = BASE_DIR.getCanonicalFile();
            String relative = documentId.equals(ROOT_ID) ? "" : documentId.substring(ROOT_ID.length() + 1);
            File file = new File(base, relative).getCanonicalFile();
            if (!file.equals(base) && !file.getPath().startsWith(base.getPath() + File.separator))
                throw new FileNotFoundException("Document is outside Cara storage");
            return file;
        } catch (IOException error) {
            throw new FileNotFoundException("Cannot resolve Cara storage: " + error.getMessage());
        }
    }

    private File getFile(String documentId) throws FileNotFoundException {
        File file = checkedFile(documentId);
        if (!file.exists()) throw new FileNotFoundException("Document not found");
        return file;
    }

    private String getDocumentId(File file) throws FileNotFoundException {
        try {
            File base = BASE_DIR.getCanonicalFile();
            File resolved = file.getCanonicalFile();
            if (resolved.equals(base)) return ROOT_ID + "/";
            if (!resolved.getPath().startsWith(base.getPath() + File.separator))
                throw new FileNotFoundException("Document is outside Cara storage");
            return ROOT_ID + "/" + resolved.getPath().substring(base.getPath().length() + 1);
        } catch (IOException error) {
            throw new FileNotFoundException("Cannot resolve Cara document: " + error.getMessage());
        }
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId)
            throws FileNotFoundException {
        File parent = getFile(parentDocumentId);
        File child = getFile(documentId);
        return parent.isDirectory() && child.getPath().startsWith(parent.getPath() + File.separator);
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName)
            throws FileNotFoundException {
        File parent = getFile(parentDocumentId);
        if (!parent.isDirectory() || !parent.canWrite())
            throw new FileNotFoundException("Cara folder is not writable");
        if (displayName == null || displayName.isEmpty() || displayName.equals(".")
                || displayName.equals("..") || displayName.contains("/") || displayName.contains("\\")
                || displayName.indexOf(0) >= 0)
            throw new FileNotFoundException("Invalid document name");
        try {
            File child = new File(parent, displayName);
            boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType);
            String stem = displayName;
            String extension = "";
            if (!directory) {
                int dot = displayName.lastIndexOf('.');
                if (dot > 0) { stem = displayName.substring(0, dot); extension = displayName.substring(dot); }
            }
            for (int number = 0; number < 1000; number++) {
                if (number > 0) child = new File(parent, stem + " (" + number + ")" + extension);
                boolean created = directory ? child.mkdir() : child.createNewFile();
                if (created) return getDocumentId(child);
                if (!child.exists()) throw new IOException("Cannot create Cara document");
            }
            throw new IOException("Too many documents with this name");
        } catch (IOException error) {
            throw new FileNotFoundException(error.getMessage());
        }
    }

    @Override
    public Cursor queryRoots(String[] projection) throws FileNotFoundException {
        String title = getContext().getString(R.string.app_name);
        int flags = DocumentsContract.Root.FLAG_SUPPORTS_CREATE;
        flags |= DocumentsContract.Root.FLAG_SUPPORTS_SEARCH;
        flags |= DocumentsContract.Root.FLAG_SUPPORTS_IS_CHILD;

        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DEFAULT_ROOT_PROJECTION);
        MatrixCursor.RowBuilder row = cursor.newRow();
        row.add(DocumentsContract.Root.COLUMN_ROOT_ID, ROOT_ID);
        row.add(DocumentsContract.Root.COLUMN_FLAGS, flags);
        row.add(DocumentsContract.Root.COLUMN_TITLE, title);
        row.add(DocumentsContract.Root.COLUMN_DOCUMENT_ID, getDocumentId(BASE_DIR));
        row.add(DocumentsContract.Root.COLUMN_MIME_TYPES, "*/*");
        row.add(DocumentsContract.Root.COLUMN_AVAILABLE_BYTES, BASE_DIR.getFreeSpace());
        row.add(DocumentsContract.Root.COLUMN_ICON, R.mipmap.ic_cara);
        return cursor;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION);
        includeFile(cursor, documentId, null);
        return cursor;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION);
        File parent = getFile(parentDocumentId);
        File[] children = parent.listFiles();
        if (children == null) throw new FileNotFoundException("Cara folder cannot be read");
        for (File file : children) {
            includeFile(cursor, null, file);
        }
        return cursor;
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, @Nullable CancellationSignal signal) throws FileNotFoundException {
        File file = getFile(documentId);
        int accessMode = ParcelFileDescriptor.parseMode(mode);
        return ParcelFileDescriptor.open(file, accessMode);
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        File file = getFile(documentId);
        if (!file.delete()) {
            throw new FileNotFoundException("Failed to delete document with id " + documentId);
        }
    }

    @Override
    public String getDocumentType(String documentId) throws FileNotFoundException {
        File file = getFile(documentId);
        return getMimeType(file);
    }

    @Override
    public Cursor querySearchDocuments(String rootId, String query, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION);
        File parent = getFile(rootId);

        // This example implementation searches file names for the query and doesn't rank search
        // results, so we can stop as soon as we find a sufficient number of matches.  Other
        // implementations might rank results and use other data about files, rather than the file
        // name, to produce a match.
        LinkedList<File> pending = new LinkedList<>();
        pending.add(parent);

        int MAX_SEARCH_RESULTS = 50;
        while (!pending.isEmpty() && cursor.getCount() < MAX_SEARCH_RESULTS) {
            final File file = pending.removeFirst();
            if (file.isDirectory()) {
                File[] children = file.listFiles();
                if (children != null) Collections.addAll(pending, children);
            } else {
                if (file.getName().toLowerCase().contains(query)) {
                    includeFile(cursor, null, file);
                }
            }
        }

        return cursor;
    }

    @Override
    public boolean onCreate() {
        ROOT_ID = "root";
        BASE_DIR = getContext().getExternalFilesDir(null);
        return BASE_DIR != null;
    }

    /**
     * Add a representation of a file to a cursor.
     *
     * @param cursor the cursor to modify
     * @param documentId  the document ID representing the desired file (may be null if given file)
     * @param file   the File object representing the desired file (may be null if given docID)
     */
    private void includeFile(MatrixCursor cursor, String documentId, File file) throws FileNotFoundException {
        if (documentId == null) {
            documentId = getDocumentId(file);
        } else if (file == null) {
            file = getFile(documentId);
        }

        String mimeType = getMimeType(file);

        int flags = 0;
        if (file.isDirectory()) {
            if (file.canWrite()) {
                flags = DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE;
            }
        } else if (file.canWrite()) {
            flags = DocumentsContract.Document.FLAG_SUPPORTS_WRITE;
            flags = flags | DocumentsContract.Document.FLAG_SUPPORTS_DELETE;
        }

        MatrixCursor.RowBuilder row = cursor.newRow();
        row.add(DocumentsContract.Document.COLUMN_DOCUMENT_ID, documentId);
        row.add(DocumentsContract.Document.COLUMN_SIZE, file.length());
        row.add(DocumentsContract.Document.COLUMN_MIME_TYPE, mimeType);
        row.add(DocumentsContract.Document.COLUMN_LAST_MODIFIED, file.lastModified());
        row.add(DocumentsContract.Document.COLUMN_FLAGS, flags);
        row.add(DocumentsContract.Root.COLUMN_ICON, R.mipmap.ic_cara);

        if (file.equals(BASE_DIR)) {
            String title = getContext().getString(R.string.app_name);
            row.add(DocumentsContract.Document.COLUMN_DISPLAY_NAME, title);
        } else {
            row.add(DocumentsContract.Document.COLUMN_DISPLAY_NAME, file.getName());
        }
    }

    private static String getMimeType(File file) {
        if (file.isDirectory()) {
            return DocumentsContract.Document.MIME_TYPE_DIR;
        } else {
            final String name = file.getName();
            final int lastDot = name.lastIndexOf('.');
            if (lastDot >= 0) {
                final String extension = name.substring(lastDot + 1).toLowerCase();
                final String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
                if (mime != null) return mime;
            }
            return "application/octet-stream";
        }
    }
}