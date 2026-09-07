package main

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"os"
	"runtime/debug"

	xdraw "golang.org/x/image/draw"
)

const (
	maxID3TagSize       = 64 << 20
	maxArtworkSize      = 8 << 20
	maxArtworkMetadata  = 64 << 10
	maxArtworkDimension = 4096
	defaultCoverSize    = 98
)

type artworkLocation struct {
	offset int64
	size   int64
}

func loadTrackCover(path string) image.Image {
	file, err := os.Open(path)
	if err != nil {
		return defaultCover()
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return defaultCover()
	}
	fingerprint := fingerprintArtwork(info)
	if cover, ok := loadArtworkCache(path, fingerprint); ok {
		return cover
	}
	_ = os.Remove(artworkCachePath(path))
	location, ok := locateID3Artwork(file)
	if !ok {
		return defaultCover()
	}
	cover, ok := decodeCompactCover(file, location)
	if !ok {
		return defaultCover()
	}
	_ = saveArtworkCache(path, fingerprint, cover)
	return cover
}

func locateID3Artwork(reader io.ReaderAt) (artworkLocation, bool) {
	header := make([]byte, 10)
	if _, err := reader.ReadAt(header, 0); err != nil || !bytes.Equal(header[:3], []byte("ID3")) {
		return artworkLocation{}, false
	}
	major := header[3]
	if (major != 3 && major != 4) || header[5]&0x80 != 0 || !validSyncSafe(header[6:10]) {
		return artworkLocation{}, false
	}
	tagSize := int64(syncSafeSize(header[6:10]))
	if tagSize <= 0 || tagSize > maxID3TagSize {
		return artworkLocation{}, false
	}
	offset, tagEnd := int64(10), int64(10)+tagSize
	if header[5]&0x40 != 0 {
		next, ok := skipExtendedHeader(reader, offset, tagEnd, major)
		if !ok {
			return artworkLocation{}, false
		}
		offset = next
	}
	frameHeader := make([]byte, 10)
	for offset+10 <= tagEnd {
		if _, err := reader.ReadAt(frameHeader, offset); err != nil || frameHeader[0] == 0 {
			return artworkLocation{}, false
		}
		frameSize := int64(binary.BigEndian.Uint32(frameHeader[4:8]))
		if major == 4 {
			if !validSyncSafe(frameHeader[4:8]) {
				return artworkLocation{}, false
			}
			frameSize = int64(syncSafeSize(frameHeader[4:8]))
		}
		bodyOffset := offset + 10
		if frameSize <= 0 || frameSize > tagEnd-bodyOffset {
			return artworkLocation{}, false
		}
		if bytes.Equal(frameHeader[:4], []byte("APIC")) && frameHeader[8] == 0 && frameHeader[9] == 0 {
			if location, ok := locateAPICImage(reader, bodyOffset, frameSize); ok {
				return location, true
			}
		}
		offset = bodyOffset + frameSize
	}
	return artworkLocation{}, false
}

func skipExtendedHeader(reader io.ReaderAt, offset, tagEnd int64, major byte) (int64, bool) {
	data := make([]byte, 4)
	if _, err := reader.ReadAt(data, offset); err != nil {
		return 0, false
	}
	size := int64(binary.BigEndian.Uint32(data))
	if major == 4 {
		if !validSyncSafe(data) {
			return 0, false
		}
		size = int64(syncSafeSize(data))
	} else {
		size += 4
	}
	if size < 4 || size > tagEnd-offset {
		return 0, false
	}
	return offset + size, true
}

func locateAPICImage(reader io.ReaderAt, bodyOffset, bodySize int64) (artworkLocation, bool) {
	metadataSize := bodySize
	if metadataSize > maxArtworkMetadata {
		metadataSize = maxArtworkMetadata
	}
	metadata := make([]byte, metadataSize)
	if _, err := reader.ReadAt(metadata, bodyOffset); err != nil {
		return artworkLocation{}, false
	}
	imageStart, ok := apicImageStart(metadata)
	if !ok {
		return artworkLocation{}, false
	}
	imageSize := bodySize - int64(imageStart)
	if imageSize <= 0 || imageSize > maxArtworkSize {
		return artworkLocation{}, false
	}
	location := artworkLocation{offset: bodyOffset + int64(imageStart), size: imageSize}
	signature := make([]byte, 8)
	read, _ := reader.ReadAt(signature, location.offset)
	if !isSupportedImage(signature[:read]) {
		return artworkLocation{}, false
	}
	return location, true
}

func apicImageStart(body []byte) (int, bool) {
	if len(body) < 4 || body[0] > 3 {
		return 0, false
	}
	mimeEnd := bytes.IndexByte(body[1:], 0)
	if mimeEnd < 0 {
		return 0, false
	}
	pictureTypeOffset := mimeEnd + 2
	descriptionStart := pictureTypeOffset + 1
	if descriptionStart >= len(body) {
		return 0, false
	}
	descriptionEnd := descriptionEndOffset(body, descriptionStart, body[0])
	if descriptionEnd < 0 || descriptionEnd >= len(body) {
		return 0, false
	}
	return descriptionEnd, true
}

func decodeCompactCover(reader io.ReaderAt, location artworkLocation) (*image.Gray, bool) {
	configReader := io.NewSectionReader(reader, location.offset, location.size)
	config, _, err := image.DecodeConfig(configReader)
	if err != nil || config.Width <= 0 || config.Height <= 0 || config.Width > maxArtworkDimension || config.Height > maxArtworkDimension || int64(config.Width)*int64(config.Height) > int64(maxArtworkDimension)*int64(maxArtworkDimension) {
		return nil, false
	}
	decoded, _, err := image.Decode(io.NewSectionReader(reader, location.offset, location.size))
	if err != nil {
		return nil, false
	}
	bounds := decoded.Bounds()
	side := bounds.Dx()
	if bounds.Dy() < side {
		side = bounds.Dy()
	}
	crop := image.Rect(
		bounds.Min.X+(bounds.Dx()-side)/2,
		bounds.Min.Y+(bounds.Dy()-side)/2,
		bounds.Min.X+(bounds.Dx()+side)/2,
		bounds.Min.Y+(bounds.Dy()+side)/2,
	)
	cover := image.NewGray(image.Rect(0, 0, defaultCoverSize, defaultCoverSize))
	xdraw.ApproxBiLinear.Scale(cover, cover.Bounds(), decoded, crop, xdraw.Src, nil)
	decoded = nil
	debug.FreeOSMemory()
	return cover, true
}

func validSyncSafe(value []byte) bool {
	return len(value) == 4 && value[0]&0x80 == 0 && value[1]&0x80 == 0 && value[2]&0x80 == 0 && value[3]&0x80 == 0
}

func syncSafeSize(value []byte) uint32 {
	return uint32(value[0]&0x7f)<<21 | uint32(value[1]&0x7f)<<14 | uint32(value[2]&0x7f)<<7 | uint32(value[3]&0x7f)
}

func descriptionEndOffset(body []byte, start int, encoding byte) int {
	if encoding == 0 || encoding == 3 {
		end := bytes.IndexByte(body[start:], 0)
		if end < 0 {
			return -1
		}
		return start + end + 1
	}
	for offset := start; offset+1 < len(body); offset += 2 {
		if body[offset] == 0 && body[offset+1] == 0 {
			return offset + 2
		}
	}
	return -1
}

func isSupportedImage(data []byte) bool {
	return bytes.HasPrefix(data, []byte("\xff\xd8\xff")) || bytes.HasPrefix(data, []byte("\x89PNG\r\n\x1a\n"))
}

func defaultCover() image.Image {
	return recordArtwork(0, false)
}

func fillCover(cover *image.Gray, value uint8) {
	for index := range cover.Pix {
		cover.Pix[index] = value
	}
}

func fillCoverCircle(cover *image.Gray, centerX, centerY, radius int, value uint8) {
	radiusSquared := radius * radius
	for y := centerY - radius; y <= centerY+radius; y++ {
		for x := centerX - radius; x <= centerX+radius; x++ {
			dx, dy := x-centerX, y-centerY
			if dx*dx+dy*dy <= radiusSquared && image.Pt(x, y).In(cover.Bounds()) {
				cover.SetGray(x, y, color.Gray{Y: value})
			}
		}
	}
}
