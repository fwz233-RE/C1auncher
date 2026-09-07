package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type Picture struct {
	Path string
	Name string
}

const maxLibraryPictures = 4096
const maxLibraryDirectories = 1024

var supportedPictureExtensions = map[string]struct{}{
	".jpg": {}, ".jpeg": {}, ".png": {}, ".gif": {}, ".raw": {},
}

func isSupportedPicture(path string) bool {
	_, ok := supportedPictureExtensions[strings.ToLower(filepath.Ext(path))]
	return ok
}

func scanPictures(root string) ([]Picture, error) { return scanPictureRoots([]string{root}) }

// Include the fixed raw wallpaper and conventional Pictures imports, without
// moving user files or recursively scanning the entire device storage.
func scanPictureDirectories(root string) ([]Picture, error) {
	roots := []string{root}
	if filepath.Clean(root) == filepath.Clean(defaultPicturesDir) {
		roots = append(roots, filepath.Join(filepath.Dir(root), "pic"), filepath.Join(filepath.Dir(root), "Pictures"))
	}
	return scanPictureRoots(roots)
}

// Read directory entries in bounded batches. A broken/inaccessible sibling must
// not hide readable pictures. Refuse symlinks and cap both traversal and results.
func scanPictureRoots(roots []string) ([]Picture, error) {
	pictures := make([]Picture, 0)
	var warnings []error
	var identities []os.FileInfo
	visited := 0
	var walk func(string, int)
	walk = func(dir string, depth int) {
		if depth > 16 || visited >= maxLibraryDirectories || len(pictures) >= maxLibraryPictures {
			if len(warnings) == 0 {
				warnings = append(warnings, errors.New("图片目录达到扫描上限"))
			}
			return
		}
		visited++
		f, err := os.Open(dir)
		if err != nil {
			warnings = append(warnings, err)
			return
		}
		defer f.Close()
		for {
			entries, err := f.ReadDir(64)
			for _, entry := range entries {
				if len(pictures) >= maxLibraryPictures {
					warnings = append(warnings, errors.New("图片数量达到4096张上限"))
					return
				}
				if entry.Type()&os.ModeSymlink != 0 {
					continue
				}
				path := filepath.Join(dir, entry.Name())
				if len(path) > 1024 {
					continue
				}
				if entry.IsDir() {
					walk(path, depth+1)
					continue
				}
				if !entry.Type().IsRegular() || !isSupportedPicture(entry.Name()) {
					continue
				}
				pictures = append(pictures, Picture{Path: path, Name: entry.Name()})
			}
			if err != nil {
				if !errors.Is(err, io.EOF) {
					warnings = append(warnings, err)
				}
				return
			}
		}
	}
	for _, root := range roots {
		info, err := os.Lstat(root)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			warnings = append(warnings, err)
			continue
		}
		if !info.IsDir() {
			warnings = append(warnings, fmt.Errorf("not a picture directory: %s", root))
			continue
		}
		duplicate := false
		for _, existing := range identities {
			if os.SameFile(info, existing) {
				duplicate = true
				break
			}
		}
		if duplicate {
			continue
		}
		identities = append(identities, info)
		walk(root, 0)
	}
	return sortPictures(pictures), errors.Join(warnings...)
}

func sortPictures(pictures []Picture) []Picture {
	sort.SliceStable(pictures, func(i, j int) bool {
		left, right := strings.ToLower(pictures[i].Path), strings.ToLower(pictures[j].Path)
		if left == right {
			return pictures[i].Path < pictures[j].Path
		}
		return left < right
	})
	return pictures
}

func pictureName(name string) string { return strings.TrimSuffix(name, filepath.Ext(name)) }
func deletePicture(p Picture) error  { return os.Remove(p.Path) }
