package main

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"image"
	"io"
	"os"
	"path/filepath"
)

const (
	artworkCacheVersion    = 1
	artworkCacheHeaderSize = 36
	artworkCacheSuffix     = ".cover-cache"
)

var artworkCacheMagic = [8]byte{'C', '1', 'C', 'O', 'V', 'E', 'R', 0}

type artworkFingerprint struct {
	size    int64
	modTime int64
}

func fingerprintArtwork(info os.FileInfo) artworkFingerprint {
	return artworkFingerprint{size: info.Size(), modTime: info.ModTime().UnixNano()}
}

func artworkCachePath(trackPath string) string {
	return trackPath + artworkCacheSuffix
}

func loadArtworkCache(trackPath string, source artworkFingerprint) (*image.Gray, bool) {
	file, err := os.Open(artworkCachePath(trackPath))
	if err != nil {
		return nil, false
	}
	defer file.Close()
	info, err := file.Stat()
	expectedSize := int64(artworkCacheHeaderSize + defaultCoverSize*defaultCoverSize)
	if err != nil || info.Size() != expectedSize {
		return nil, false
	}
	header := make([]byte, artworkCacheHeaderSize)
	if _, err := io.ReadFull(file, header); err != nil || !validArtworkCacheHeader(header, source) {
		return nil, false
	}
	cover := image.NewGray(image.Rect(0, 0, defaultCoverSize, defaultCoverSize))
	if _, err := io.ReadFull(file, cover.Pix); err != nil ||
		binary.LittleEndian.Uint32(header[32:36]) != crc32.ChecksumIEEE(cover.Pix) {
		return nil, false
	}
	return cover, true
}

func validArtworkCacheHeader(header []byte, source artworkFingerprint) bool {
	return len(header) == artworkCacheHeaderSize &&
		bytes.Equal(header[:8], artworkCacheMagic[:]) &&
		binary.LittleEndian.Uint32(header[8:12]) == artworkCacheVersion &&
		binary.LittleEndian.Uint16(header[12:14]) == defaultCoverSize &&
		binary.LittleEndian.Uint16(header[14:16]) == defaultCoverSize &&
		int64(binary.LittleEndian.Uint64(header[16:24])) == source.size &&
		int64(binary.LittleEndian.Uint64(header[24:32])) == source.modTime
}

func saveArtworkCache(trackPath string, source artworkFingerprint, cover *image.Gray) error {
	if cover == nil || cover.Bounds() != image.Rect(0, 0, defaultCoverSize, defaultCoverSize) ||
		cover.Stride != defaultCoverSize || len(cover.Pix) != defaultCoverSize*defaultCoverSize {
		return os.ErrInvalid
	}
	header := make([]byte, artworkCacheHeaderSize)
	copy(header[:8], artworkCacheMagic[:])
	binary.LittleEndian.PutUint32(header[8:12], artworkCacheVersion)
	binary.LittleEndian.PutUint16(header[12:14], defaultCoverSize)
	binary.LittleEndian.PutUint16(header[14:16], defaultCoverSize)
	binary.LittleEndian.PutUint64(header[16:24], uint64(source.size))
	binary.LittleEndian.PutUint64(header[24:32], uint64(source.modTime))
	binary.LittleEndian.PutUint32(header[32:36], crc32.ChecksumIEEE(cover.Pix))

	directory := filepath.Dir(trackPath)
	temporary, err := os.CreateTemp(directory, ".cover-cache-*.tmp")
	if err != nil {
		return err
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if _, err = temporary.Write(header); err == nil {
		_, err = temporary.Write(cover.Pix)
	}
	if err == nil {
		err = temporary.Sync()
	}
	if closeErr := temporary.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return replaceArtworkCache(temporaryPath, artworkCachePath(trackPath))
}

func replaceArtworkCache(temporaryPath, targetPath string) error {
	if err := os.Rename(temporaryPath, targetPath); err == nil {
		return nil
	}
	if err := os.Remove(targetPath); err != nil && !os.IsNotExist(err) {
		return err
	}
	return os.Rename(temporaryPath, targetPath)
}
