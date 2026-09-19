package main

import (
	"archive/tar"
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/klauspost/compress/zstd"
)

type testArchiveEntry struct {
	name string
	body  []byte
}

func writeTestArchive(t *testing.T, entries []testArchiveEntry) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "bundle.tar.zst")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	encoder, err := zstd.NewWriter(f)
	if err != nil {
		f.Close()
		t.Fatal(err)
	}
	tarWriter := tar.NewWriter(encoder)
	for _, entry := range entries {
		header := &tar.Header{
			Name:     entry.name,
			Mode:     0600,
			Size:     int64(len(entry.body)),
			Typeflag: tar.TypeReg,
		}
		if err := tarWriter.WriteHeader(header); err != nil {
			t.Fatal(err)
		}
		if _, err := tarWriter.Write(entry.body); err != nil {
			t.Fatal(err)
		}
	}
	if err := tarWriter.Close(); err != nil {
		t.Fatal(err)
	}
	if err := encoder.Close(); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestSafePathRejectsTraversalAndWindowsForms(t *testing.T) {
	for _, path := range []string{"", "/absolute", `..\\escape`, "a/../b", "C:drive", "a//b"} {
		if safePath(path) {
			t.Errorf("safePath(%q) accepted an unsafe path", path)
		}
	}
	if !safePath("bin/appsandbox-agent") {
		t.Fatal("safePath rejected a valid relative path")
	}
}

func TestScanArchiveRejectsOversizedMetadata(t *testing.T) {
	manifest := bytes.Repeat([]byte{'m'}, maxManifestBytes+1)
	bundle := writeTestArchive(t, []testArchiveEntry{{name: "manifest.json", body: manifest}})
	if _, err := scanArchive(bundle, nil, true); err == nil {
		t.Fatal("oversized manifest was accepted")
	}

	signature := bytes.Repeat([]byte{'s'}, maxSignature+1)
	bundle = writeTestArchive(t, []testArchiveEntry{
		{name: "manifest.json", body: []byte(`{}`)},
		{name: "manifest.sig", body: signature},
	})
	if _, err := scanArchive(bundle, nil, true); err == nil {
		t.Fatal("oversized signature was accepted")
	}
}

func TestScanArchiveRejectsTooManyEntriesAndDuplicates(t *testing.T) {
	entries := make([]testArchiveEntry, 0, maxArchiveFiles+1)
	for i := 0; i < maxArchiveFiles+1; i++ {
		entries = append(entries, testArchiveEntry{
			name: fmt.Sprintf("payload/file-%04d", i),
		})
	}
	bundle := writeTestArchive(t, entries)
	if _, err := scanArchive(bundle, nil, true); err == nil {
		t.Fatal("archive with too many entries was accepted")
	}

	bundle = writeTestArchive(t, []testArchiveEntry{
		{name: "payload/one", body: []byte("a")},
		{name: "payload/one", body: []byte("b")},
	})
	if _, err := scanArchive(bundle, nil, true); err == nil {
		t.Fatal("duplicate archive path was accepted")
	}
}
