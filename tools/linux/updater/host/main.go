// appsandbox-guest-bundle-verifier is the independent Windows Host preflight
// verifier. It contains its zstd decoder and streams the archive twice: the
// first pass validates the archive shape and signed manifest, and the second
// pass hashes payloads without retaining them in memory.
package main

import (
	"archive/tar"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path"
	"regexp"
	"strconv"
	"strings"

	"github.com/klauspost/compress/zstd"
)

var releasePublicKeyHex = ""

const (
	maxBundle        = int64(512) * 1024 * 1024
	maxUncompressed  = int64(2) * 1024 * 1024 * 1024
	maxFile          = int64(512) * 1024 * 1024
	maxArchiveFiles  = 4096
	maxManifestBytes = 1 * 1024 * 1024
	maxSignature     = 4 * 1024
	maxPathBytes     = 4096
	protocol         = 1
	updaterVersion   = "1.0.0"
)

type manifest struct {
	Schema                  int          `json:"schema"`
	Kind                    string       `json:"kind"`
	Version                 string       `json:"version"`
	Arch                    string       `json:"arch"`
	OS                      string       `json:"os"`
	HostProtocolMin         int          `json:"host_protocol_min"`
	HostProtocolMax         int          `json:"host_protocol_max"`
	UpdaterMinVersion       string       `json:"updater_min_version"`
	GraphicsVersion         string       `json:"graphics_version"`
	KernelComponentsPresent bool         `json:"kernel_components_present"`
	Files                   []fileRecord `json:"files"`
}

type fileRecord struct {
	Path      string `json:"path"`
	SHA       string `json:"sha256"`
	Size      int64  `json:"size"`
	Mode      uint32 `json:"mode"`
	Component string `json:"component"`
}

type archiveEntry struct {
	size int64
}

type archiveIndex struct {
	manifest  []byte
	signature []byte
	payload   map[string]archiveEntry
}

var semverRE = regexp.MustCompile(`^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$`)

func safePath(p string) bool {
	if p == "" || len(p) > maxPathBytes || strings.HasPrefix(p, "/") ||
		strings.HasPrefix(p, `\`) || strings.Contains(p, `\`) || strings.Contains(p, ":") ||
		path.Clean(p) != p {
		return false
	}
	for _, part := range strings.Split(p, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

func rejectKernel(p string) bool {
	l := strings.ToLower(p)
	return strings.Contains(l, "dxgkrnl") || strings.Contains(l, "asb_drm.ko") ||
		strings.Contains(l, "modules/")
}

func validSHA(s string) bool {
	if len(s) != 64 {
		return false
	}
	_, e := hex.DecodeString(s)
	return e == nil
}

func parseVersion(s string) bool { return semverRE.MatchString(s) }

func versionAtMost(required, supported string) bool {
	parse := func(s string) [3]int {
		var out [3]int
		parts := strings.SplitN(strings.SplitN(s, "-", 2)[0], "+", 2)[0]
		for i, p := range strings.Split(parts, ".") {
			if i < 3 {
				n, _ := strconv.Atoi(p)
				out[i] = n
			}
		}
		return out
	}
	a, b := parse(required), parse(supported)
	for i := 0; i < 3; i++ {
		if a[i] != b[i] {
			return a[i] < b[i]
		}
	}
	return true
}

func parsePublicKey() (ed25519.PublicKey, error) {
	if len(releasePublicKeyHex) != 64 {
		return nil, errors.New("release public key missing")
	}
	b, err := hex.DecodeString(releasePublicKeyHex)
	if err != nil || len(b) != ed25519.PublicKeySize {
		return nil, errors.New("release public key malformed")
	}
	allZero := true
	for _, x := range b {
		if x != 0 {
			allZero = false
			break
		}
	}
	if allZero {
		return nil, errors.New("zero release public key")
	}
	return ed25519.PublicKey(b), nil
}

func verifyOuter(bundle, expected string) error {
	st, err := os.Stat(bundle)
	if err != nil || !st.Mode().IsRegular() || st.Size() > maxBundle {
		return errors.New("bundle missing or oversized")
	}
	f, err := os.Open(bundle)
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	if _, err = io.Copy(h, io.LimitReader(f, maxBundle+1)); err != nil {
		return err
	}
	if !strings.EqualFold(hex.EncodeToString(h.Sum(nil)), expected) {
		return errors.New("outer SHA256 mismatch")
	}
	return nil
}

func readExact(r io.Reader, size int64) ([]byte, error) {
	if size < 0 || size > int64(maxManifestBytes) {
		return nil, errors.New("metadata entry oversized")
	}
	b := make([]byte, size)
	n, err := io.ReadFull(r, b)
	if err != nil || int64(n) != size {
		return nil, errors.New("truncated tar entry")
	}
	return b, nil
}

func drainExact(r io.Reader, size int64) error {
	if size < 0 {
		return errors.New("negative tar entry")
	}
	n, err := io.CopyN(io.Discard, r, size)
	if err != nil || n != size {
		return errors.New("truncated tar entry")
	}
	return nil
}

// scanArchive parses exactly one zstd/tar stream. Payload bytes are either
// drained or hashed in place, never retained.
func scanArchive(bundle string, expected map[string]fileRecord, first bool) (*archiveIndex, error) {
	f, err := os.Open(bundle)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	decoder, err := zstd.NewReader(f, zstd.WithDecoderConcurrency(1), zstd.WithDecoderLowmem(true))
	if err != nil {
		return nil, errors.New("invalid zstd stream")
	}
	defer decoder.Close()

	index := &archiveIndex{payload: make(map[string]archiveEntry)}
	seen := make(map[string]struct{})
	matched := make(map[string]struct{})
	tr := tar.NewReader(decoder)
	var total int64
	entries := 0
	for {
		h, e := tr.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return nil, errors.New("invalid tar")
		}
		entries++
		if entries > maxArchiveFiles || h.Size < 0 || h.Size > maxFile ||
			total > maxUncompressed-h.Size {
			return nil, errors.New("archive limits exceeded")
		}
		total += h.Size
		name := h.Name
		if h.Typeflag == tar.TypeDir {
			name = strings.TrimSuffix(name, "/")
		}
		if !safePath(name) {
			return nil, errors.New("unsafe tar path")
		}
		if _, ok := seen[name]; ok {
			return nil, errors.New("duplicate tar entry")
		}
		seen[name] = struct{}{}

		switch h.Typeflag {
		case tar.TypeDir:
			if h.Size != 0 {
				return nil, errors.New("invalid directory")
			}
		case tar.TypeReg, tar.TypeRegA:
			switch name {
			case "manifest.json":
				if first {
					if h.Size > maxManifestBytes {
						return nil, errors.New("manifest oversized")
					}
					index.manifest, err = readExact(tr, h.Size)
				} else {
					err = drainExact(tr, h.Size)
				}
				if err != nil {
					return nil, err
				}
			case "manifest.sig":
				if h.Size <= 0 || h.Size > maxSignature {
					return nil, errors.New("signature oversized")
				}
				if first {
					index.signature, err = readExact(tr, h.Size)
				} else {
					err = drainExact(tr, h.Size)
				}
				if err != nil {
					return nil, err
				}
			default:
				if !strings.HasPrefix(name, "payload/") {
					return nil, errors.New("invalid archive entry")
				}
				p := strings.TrimPrefix(name, "payload/")
				if !safePath(p) || rejectKernel(p) {
					return nil, errors.New("invalid payload path")
				}
				if first {
					index.payload[p] = archiveEntry{size: h.Size}
					if err := drainExact(tr, h.Size); err != nil {
						return nil, err
					}
				} else {
					rec, ok := expected[p]
					if !ok || rec.Size != h.Size {
						return nil, errors.New("payload set or size changed")
					}
					digest := sha256.New()
					if _, err := io.CopyN(digest, tr, h.Size); err != nil {
						return nil, errors.New("truncated payload")
					}
					if !strings.EqualFold(hex.EncodeToString(digest.Sum(nil)), rec.SHA) {
						return nil, errors.New("payload hash mismatch")
					}
					matched[p] = struct{}{}
				}
			}
		default:
			return nil, errors.New("forbidden tar entry")
		}
	}
	if first {
		if len(index.manifest) == 0 || len(index.signature) == 0 {
			return nil, errors.New("manifest or signature missing")
		}
		return index, nil
	}
	if len(matched) != len(expected) {
		return nil, errors.New("payload missing")
	}
	return nil, nil
}

func verifyManifest(mb, sig []byte, index *archiveIndex, key ed25519.PublicKey) (map[string]fileRecord, error) {
	if !ed25519.Verify(key, mb, sig) {
		trim := strings.TrimSpace(string(sig))
		decoded, e := hex.DecodeString(trim)
		if e != nil || !ed25519.Verify(key, mb, decoded) {
			return nil, errors.New("invalid manifest signature")
		}
	}
	var m manifest
	if json.Unmarshal(mb, &m) != nil || m.Schema != 1 || (m.Kind != "runtime" && m.Kind != "graphics") ||
		m.Arch != "amd64" || m.OS != "ubuntu-26.04" || m.HostProtocolMin > protocol || m.HostProtocolMax < protocol ||
		!parseVersion(m.Version) || !parseVersion(m.UpdaterMinVersion) || !versionAtMost(m.UpdaterMinVersion, updaterVersion) ||
		m.KernelComponentsPresent || len(m.Files) > maxArchiveFiles {
		return nil, errors.New("manifest policy rejected")
	}
	declared := make(map[string]fileRecord, len(m.Files))
	for _, f := range m.Files {
		if !safePath(f.Path) || rejectKernel(f.Path) || !validSHA(f.SHA) || f.Size < 0 || f.Size > maxFile ||
			f.Mode > 07777 || f.Component == "" || strings.SplitN(f.Path, "/", 2)[0] != f.Component {
			return nil, errors.New("manifest file rejected")
		}
		if _, ok := declared[f.Path]; ok {
			return nil, errors.New("duplicate manifest path")
		}
		entry, ok := index.payload[f.Path]
		if !ok || entry.size != f.Size {
			return nil, errors.New("manifest payload mismatch")
		}
		declared[f.Path] = f
	}
	if len(declared) != len(index.payload) {
		return nil, errors.New("undeclared payload")
	}
	_, mesa := declared["graphics/wsl-mesa.tar.zst"]
	if (m.GraphicsVersion != "") != mesa || (mesa && !parseVersion(m.GraphicsVersion)) {
		return nil, errors.New("graphics metadata mismatch")
	}
	_, d3d12Binary := declared["libexec/appsandbox-display-d3d12"]
	_, d3d12Unit := declared["systemd/appsandbox-display-d3d12.service"]
	if d3d12Binary != d3d12Unit {
		return nil, errors.New("incomplete D3D12 runtime")
	}
	if m.Kind == "runtime" {
		for _, p := range []string{"bin/appsandbox-agent", "bin/appsandbox-display", "bin/appsandbox-input", "bin/appsandbox-audio", "bin/appsandbox-clipboard", "systemd/appsandbox-agent.service", "systemd/appsandbox-display.service", "systemd/appsandbox-input.service", "systemd/appsandbox-audio.service"} {
			if _, ok := declared[p]; !ok {
				return nil, errors.New("incomplete runtime")
			}
		}
	} else {
		if !mesa || m.GraphicsVersion == "" || !parseVersion(m.GraphicsVersion) {
			return nil, errors.New("incomplete graphics bundle")
		}
		for p := range declared {
			if !strings.HasPrefix(p, "graphics/") && !strings.HasPrefix(p, "config/") && !strings.HasPrefix(p, "gnome/") {
				return nil, errors.New("runtime payload in graphics bundle")
			}
		}
	}
	return declared, nil
}

func main() {
	if len(os.Args) != 4 || os.Args[1] != "--verify-bundle" || !validSHA(os.Args[3]) {
		os.Exit(2)
	}
	if e := verifyOuter(os.Args[2], os.Args[3]); e != nil {
		os.Exit(1)
	}
	key, e := parsePublicKey()
	if e != nil {
		os.Exit(1)
	}
	index, e := scanArchive(os.Args[2], nil, true)
	if e != nil {
		os.Exit(1)
	}
	declared, e := verifyManifest(index.manifest, index.signature, index, key)
	if e != nil {
		os.Exit(1)
	}
	if _, e = scanArchive(os.Args[2], declared, false); e != nil {
		os.Exit(1)
	}
}
