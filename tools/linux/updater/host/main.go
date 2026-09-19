// appsandbox-guest-bundle-verifier is the independent Windows Host preflight
// verifier. It uses fixed argv for zstd, parses tar itself, and performs the
// Ed25519 and manifest checks independently of the Guest updater.
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
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

var releasePublicKeyHex = ""

const (
	maxBundle       = int64(512) * 1024 * 1024
	maxUncompressed = int64(2) * 1024 * 1024 * 1024
	maxFile         = int64(512) * 1024 * 1024
	protocol        = 1
	updaterVersion  = "1.0.0"
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

var semverRE = regexp.MustCompile(`^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$`)

func safePath(p string) bool {
	return p != "" && !strings.HasPrefix(p, "/") && !strings.HasPrefix(p, `\`) &&
		!strings.Contains(p, `\`) && !strings.Contains(p, "..") && path.Clean(p) == p
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
	if err != nil || st.Size() > maxBundle {
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

func readArchive(bundle string) ([]byte, []byte, map[string][]byte, error) {
	// No shell is involved: exec.Command receives a fixed argv vector.
	zstd := "zstd.exe"
	if exe, e := os.Executable(); e == nil {
		candidate := filepath.Join(filepath.Dir(exe), "zstd.exe")
		if _, e = os.Stat(candidate); e == nil {
			zstd = candidate
		}
	}
	cmd := exec.Command(zstd, "--quiet", "--decompress", "--stdout", bundle)
	rc, err := cmd.StdoutPipe()
	if err != nil {
		return nil, nil, nil, err
	}
	if err = cmd.Start(); err != nil {
		return nil, nil, nil, err
	}
	waited := false
	defer func() {
		if !waited && cmd.Process != nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
	}()
	tr := tar.NewReader(rc)
	var manifestBytes, signature []byte
	files := map[string][]byte{}
	var total int64
	for {
		h, e := tr.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			_ = cmd.Process.Kill()
			return nil, nil, nil, errors.New("invalid tar")
		}
		name := h.Name
		if h.Typeflag == tar.TypeDir {
			name = strings.TrimSuffix(name, "/")
		}
		if h.Size < 0 || h.Size > maxFile || total > maxUncompressed-h.Size || !safePath(name) {
			_ = cmd.Process.Kill()
			return nil, nil, nil, errors.New("unsafe tar entry")
		}
		total += h.Size
		switch h.Typeflag {
		case tar.TypeReg, tar.TypeRegA:
			b, e := io.ReadAll(io.LimitReader(tr, h.Size+1))
			if e != nil || int64(len(b)) != h.Size {
				return nil, nil, nil, errors.New("truncated tar entry")
			}
			switch name {
			case "manifest.json":
				if manifestBytes != nil {
					return nil, nil, nil, errors.New("duplicate manifest")
				}
				manifestBytes = b
			case "manifest.sig":
				if signature != nil {
					return nil, nil, nil, errors.New("duplicate signature")
				}
				signature = b
			default:
				if !strings.HasPrefix(name, "payload/") || !safePath(strings.TrimPrefix(name, "payload/")) {
					return nil, nil, nil, errors.New("invalid payload path")
				}
				p := strings.TrimPrefix(name, "payload/")
				if _, ok := files[p]; ok {
					return nil, nil, nil, errors.New("duplicate payload")
				}
				files[p] = b
			}
		case tar.TypeDir:
			if h.Size != 0 {
				return nil, nil, nil, errors.New("invalid directory")
			}
		case tar.TypeSymlink, tar.TypeLink, tar.TypeBlock, tar.TypeChar, tar.TypeFifo, tar.TypeXGlobalHeader, tar.TypeXHeader, tar.TypeGNULongName, tar.TypeGNULongLink:
			return nil, nil, nil, errors.New("forbidden tar entry")
		default:
			return nil, nil, nil, errors.New("unsupported tar entry")
		}
	}
	err = cmd.Wait()
	waited = true
	if err != nil || manifestBytes == nil || signature == nil {
		return nil, nil, nil, errors.New("invalid zstd stream")
	}
	return manifestBytes, signature, files, nil
}

func verifyManifest(mb, sig []byte, files map[string][]byte, key ed25519.PublicKey) error {
	if !ed25519.Verify(key, mb, sig) { // raw detached signature is the release format
		trim := strings.TrimSpace(string(sig))
		decoded, e := hex.DecodeString(trim)
		if e != nil || !ed25519.Verify(key, mb, decoded) {
			return errors.New("invalid manifest signature")
		}
	}
	var m manifest
	if json.Unmarshal(mb, &m) != nil || m.Schema != 1 || (m.Kind != "runtime" && m.Kind != "graphics") ||
		m.Arch != "amd64" || m.OS != "ubuntu-26.04" || m.HostProtocolMin > protocol || m.HostProtocolMax < protocol ||
		!parseVersion(m.Version) || !parseVersion(m.UpdaterMinVersion) || !versionAtMost(m.UpdaterMinVersion, updaterVersion) || m.KernelComponentsPresent {
		return errors.New("manifest policy rejected")
	}
	declared := map[string]fileRecord{}
	for _, f := range m.Files {
		if !safePath(f.Path) || rejectKernel(f.Path) || !validSHA(f.SHA) || f.Size < 0 || f.Size > maxFile ||
			f.Mode > 07777 || f.Component == "" || strings.SplitN(f.Path, "/", 2)[0] != f.Component {
			return errors.New("manifest file rejected")
		}
		if _, ok := declared[f.Path]; ok {
			return errors.New("duplicate manifest path")
		}
		declared[f.Path] = f
		b, ok := files[f.Path]
		digest := sha256.Sum256(b)
		if !ok || int64(len(b)) != f.Size || !strings.EqualFold(hex.EncodeToString(digest[:]), f.SHA) {
			return errors.New("payload hash mismatch")
		}
	}
	if len(declared) != len(files) {
		return errors.New("undeclared payload")
	}
	_, mesa := declared["graphics/wsl-mesa.tar.zst"]
	if (m.GraphicsVersion != "") != mesa || (mesa && !parseVersion(m.GraphicsVersion)) {
		return errors.New("graphics metadata mismatch")
	}
	_, d3d12Binary := declared["libexec/appsandbox-display-d3d12"]
	_, d3d12Unit := declared["systemd/appsandbox-display-d3d12.service"]
	if d3d12Binary != d3d12Unit {
		return errors.New("incomplete D3D12 runtime")
	}
	if m.Kind == "runtime" {
		for _, p := range []string{"bin/appsandbox-agent", "bin/appsandbox-display", "bin/appsandbox-input", "bin/appsandbox-audio", "bin/appsandbox-clipboard", "systemd/appsandbox-agent.service", "systemd/appsandbox-display.service", "systemd/appsandbox-input.service", "systemd/appsandbox-audio.service"} {
			if _, ok := declared[p]; !ok {
				return errors.New("incomplete runtime")
			}
		}
	} else {
		if _, ok := declared["graphics/wsl-mesa.tar.zst"]; !ok || m.GraphicsVersion == "" || !parseVersion(m.GraphicsVersion) {
			return errors.New("incomplete graphics bundle")
		}
		for p := range declared {
			if !strings.HasPrefix(p, "graphics/") && !strings.HasPrefix(p, "config/") && !strings.HasPrefix(p, "gnome/") {
				return errors.New("runtime payload in graphics bundle")
			}
		}
	}
	return nil
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
	mb, sig, files, e := readArchive(os.Args[2])
	if e != nil || verifyManifest(mb, sig, files, key) != nil {
		os.Exit(1)
	}
}
