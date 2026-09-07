package main

import (
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf8"
)

type Book struct {
	Path string
	Name string
}

func ScanLibrary(root string) ([]Book, error) {
	books := make([]Book, 0)
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() || !strings.EqualFold(filepath.Ext(entry.Name()), ".txt") {
			return nil
		}
		books = append(books, Book{Path: path, Name: bookDisplayName(path, entry.Name())})
		return nil
	})
	if err != nil {
		return nil, err
	}
	sort.SliceStable(books, func(i, j int) bool {
		left, right := strings.ToLower(books[i].Path), strings.ToLower(books[j].Path)
		if left == right {
			return books[i].Path < books[j].Path
		}
		return left < right
	})
	return books, nil
}

func bookDisplayName(path, fallback string) string {
	data, err := os.ReadFile(path + ".name")
	if err != nil || len(data) == 0 || len(data) > 4096 || !utf8.Valid(data) {
		return fallback
	}
	name := strings.TrimSpace(string(data))
	if name == "" || strings.ContainsAny(name, "\x00\r\n") {
		return fallback
	}
	return name
}
