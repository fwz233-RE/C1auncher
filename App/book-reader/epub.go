package main

import (
	"archive/zip"
	"bytes"
	"encoding/xml"
	"errors"
	"fmt"
	"html"
	"io"
	"net/url"
	"path"
	"strings"
	"unicode"
	"unicode/utf8"
)

const (
	maxEPUBEntries                 = 10000
	maxEPUBEntryBytes       uint64 = 32 << 20
	maxEPUBExpandedBytes    uint64 = 256 << 20
	maxEPUBCompressionRatio uint64 = 1000
	maxEPUBMetadataBytes           = 4 << 20
)

type epubArchive struct {
	files     map[string]*zip.File
	readBytes uint64
}

func openEPUBArchive(reader *zip.ReadCloser) (*epubArchive, error) {
	if len(reader.File) > maxEPUBEntries {
		return nil, errors.New("EPUB has too many ZIP entries")
	}
	archive := &epubArchive{files: make(map[string]*zip.File)}
	var total uint64
	for _, file := range reader.File {
		name := strings.TrimSuffix(file.Name, "/")
		if name == "" || strings.ContainsAny(name, "\\\x00:") || strings.HasPrefix(name, "/") || path.Clean(name) != name || name == ".." || strings.HasPrefix(name, "../") {
			return nil, errors.New("unsafe EPUB ZIP entry path")
		}
		if file.FileInfo().IsDir() {
			continue
		}
		if !file.Mode().IsRegular() {
			return nil, errors.New("EPUB ZIP contains a non-regular entry")
		}
		if _, exists := archive.files[name]; exists {
			return nil, errors.New("duplicate EPUB ZIP entry")
		}
		if file.Flags&1 != 0 {
			return nil, errors.New("encrypted EPUB ZIP is unsupported")
		}
		if file.UncompressedSize64 > maxEPUBEntryBytes || file.UncompressedSize64 > maxEPUBExpandedBytes-total {
			return nil, errors.New("EPUB expansion exceeds size limit")
		}
		if file.UncompressedSize64 > 1<<20 && (file.CompressedSize64 == 0 || file.UncompressedSize64/file.CompressedSize64 > maxEPUBCompressionRatio) {
			return nil, errors.New("EPUB ZIP compression ratio exceeds limit")
		}
		total += file.UncompressedSize64
		archive.files[name] = file
	}
	return archive, nil
}

func (archive *epubArchive) read(name string, limit int64) ([]byte, error) {
	file, ok := archive.files[name]
	if !ok {
		return nil, fmt.Errorf("EPUB resource missing: %s", name)
	}
	if file.UncompressedSize64 > uint64(limit) {
		return nil, errors.New("EPUB resource exceeds size limit")
	}
	stream, err := file.Open()
	if err != nil {
		return nil, err
	}
	defer stream.Close()
	data, err := io.ReadAll(io.LimitReader(stream, limit+1))
	if err != nil {
		return nil, fmt.Errorf("EPUB ZIP resource: %w", err)
	}
	if int64(len(data)) > limit || uint64(len(data)) > maxEPUBExpandedBytes-archive.readBytes {
		return nil, errors.New("EPUB read expansion exceeds limit")
	}
	archive.readBytes += uint64(len(data))
	return data, nil
}

// Resolve internal URLs without ever mapping archive filenames to host paths.
// Parent segments are legitimate in OPF links, but may not escape ZIP root.
func epubReference(base, reference string) (string, string, error) {
	u, err := url.Parse(reference)
	if err != nil || u.IsAbs() || u.Host != "" || u.RawQuery != "" || strings.ContainsAny(u.Path, "\\\x00:") || strings.HasPrefix(u.Path, "/") {
		return "", "", errors.New("unsafe or remote EPUB reference")
	}
	resolved := base
	if u.Path != "" {
		resolved = path.Join(path.Dir(base), u.Path)
	}
	if resolved == "." || resolved == ".." || strings.HasPrefix(resolved, "../") {
		return "", "", errors.New("EPUB reference escapes ZIP root")
	}
	return resolved, u.Fragment, nil
}

type epubPackage struct {
	Manifest []struct {
		ID         string `xml:"id,attr"`
		Href       string `xml:"href,attr"`
		Media      string `xml:"media-type,attr"`
		Properties string `xml:"properties,attr"`
	} `xml:"manifest>item"`
	Spine struct {
		TOC   string `xml:"toc,attr"`
		Items []struct {
			IDRef  string `xml:"idref,attr"`
			Linear string `xml:"linear,attr"`
		} `xml:"itemref"`
	} `xml:"spine"`
}

func openEPUBDocument(bookPath string, mark BookFingerprint) (*Document, error) {
	reader, err := zip.OpenReader(bookPath)
	if err != nil {
		return nil, fmt.Errorf("open EPUB ZIP: %w", err)
	}
	defer reader.Close()
	archive, err := openEPUBArchive(reader)
	if err != nil {
		return nil, err
	}
	if _, encrypted := archive.files["META-INF/encryption.xml"]; encrypted {
		return nil, errors.New("encrypted EPUB resources are unsupported")
	}
	container, err := archive.read("META-INF/container.xml", maxEPUBMetadataBytes)
	if err != nil {
		return nil, err
	}
	var root struct {
		Files []struct {
			Path  string `xml:"full-path,attr"`
			Media string `xml:"media-type,attr"`
		} `xml:"rootfiles>rootfile"`
	}
	if err = xml.Unmarshal(container, &root); err != nil {
		return nil, fmt.Errorf("EPUB container XML: %w", err)
	}
	packagePath := ""
	for _, file := range root.Files {
		if file.Media == "application/oebps-package+xml" || file.Media == "" {
			packagePath, _, err = epubReference("container.xml", file.Path)
			break
		}
	}
	if err != nil {
		return nil, err
	}
	if packagePath == "" {
		return nil, errors.New("EPUB container has no package")
	}
	raw, err := archive.read(packagePath, maxEPUBMetadataBytes)
	if err != nil {
		return nil, err
	}
	var pack epubPackage
	if err = xml.Unmarshal(raw, &pack); err != nil {
		return nil, fmt.Errorf("EPUB package XML: %w", err)
	}
	if len(pack.Spine.Items) == 0 || len(pack.Spine.Items) > maxEPUBEntries {
		return nil, errors.New("EPUB has no usable spine")
	}
	manifest := make(map[string]int)
	for i, item := range pack.Manifest {
		if item.ID == "" {
			return nil, errors.New("EPUB manifest item has no ID")
		}
		if _, ok := manifest[item.ID]; ok {
			return nil, errors.New("duplicate EPUB manifest ID")
		}
		manifest[item.ID] = i
	}
	labels := make(map[string]string)
	for _, item := range pack.Manifest {
		nav := strings.Contains(" "+item.Properties+" ", " nav ")
		ncx := item.ID == pack.Spine.TOC && item.Media == "application/x-dtbncx+xml"
		if !nav && !ncx {
			continue
		}
		navPath, _, err := epubReference(packagePath, item.Href)
		if err != nil {
			return nil, err
		}
		navData, err := archive.read(navPath, maxEPUBMetadataBytes)
		if err != nil {
			return nil, err
		}
		if ncx {
			err = readNCXLabels(navData, navPath, labels)
		} else {
			err = readNavigationLabels(navData, navPath, labels)
		}
		if err != nil {
			return nil, err
		}
	}
	var chapters []Chapter
	var size int64
	err = writePrivateCache(normalizedDocumentPath(bookPath, mark), func(w io.Writer) error {
		for _, ref := range pack.Spine.Items {
			i, ok := manifest[ref.IDRef]
			if !ok {
				return errors.New("EPUB spine references a missing manifest item")
			}
			item := pack.Manifest[i]
			if ref.Linear == "no" {
				continue
			}
			if item.Media != "application/xhtml+xml" && item.Media != "text/html" {
				return errors.New("EPUB spine has unsupported content type")
			}
			contentPath, _, err := epubReference(packagePath, item.Href)
			if err != nil {
				return err
			}
			data, err := archive.read(contentPath, int64(maxEPUBEntryBytes))
			if err != nil {
				return err
			}
			sections, err := extractEPUBText(data, contentPath, labels)
			if err != nil {
				return fmt.Errorf("EPUB XHTML: %w", err)
			}
			for _, section := range sections {
				if strings.TrimSpace(section.Text) == "" {
					continue
				}
				text := section.Text + "\n"
				if size+int64(len(text)) > maxNormalizedTextBytes {
					return errors.New("EPUB text exceeds size limit")
				}
				start := size
				n, err := io.WriteString(w, text)
				if err != nil {
					return err
				}
				size += int64(n)
				if len(chapters) >= 50000 {
					return errors.New("EPUB chapter count exceeds limit")
				}
				chapters = append(chapters, Chapter{Title: section.Title, Start: start, End: size, HasBody: true})
			}
		}
		if len(chapters) == 0 {
			return errors.New("EPUB spine contains no readable text")
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	return finishNormalizedDocument(bookPath, mark, size, chapters)
}

func epubXML(data []byte) *xml.Decoder {
	decoder := xml.NewDecoder(bytes.NewReader(data))
	decoder.Strict = false
	decoder.AutoClose = xml.HTMLAutoClose
	decoder.Entity = make(map[string]string)
	// Resolve only standard HTML entities, never DTD declarations or URLs.
	// Iterate rather than retaining every entity match from a large chapter.
	for len(data) > 0 {
		start := bytes.IndexByte(data, '&')
		if start < 0 {
			break
		}
		data = data[start+1:]
		end := bytes.IndexByte(data[:min(len(data), 64)], ';')
		if end < 0 {
			continue
		}
		name := string(data[:end])
		data = data[end+1:]
		if name == "" || name[0] == '#' {
			continue
		}
		entity := "&" + name + ";"
		if decoded := html.UnescapeString(entity); decoded != entity && utf8.RuneCountInString(decoded) <= 2 {
			decoder.Entity[name] = decoded
		}
	}
	return decoder
}
func xmlAttribute(element xml.StartElement, name string) string {
	for _, attr := range element.Attr {
		if attr.Name.Local == name {
			return attr.Value
		}
	}
	return ""
}

func epubLabelTitle(title string) string {
	if len(title) > 2048 {
		cut := 2048
		for cut > 0 && !utf8.RuneStart(title[cut]) {
			cut--
		}
		title = title[:cut]
	}
	title = strings.Join(strings.Fields(title), " ")
	count := 0
	for offset := range title {
		if count == 256 {
			return title[:offset]
		}
		count++
	}
	return title
}

func addEPUBLabel(labels map[string]string, base, href, title string) error {
	target, fragment, err := epubReference(base, href)
	if err != nil {
		return err
	}
	title = epubLabelTitle(title)
	if title == "" {
		return nil
	}
	key := target
	if fragment != "" {
		key += "#" + fragment
	}
	if _, ok := labels[key]; !ok {
		labels[key] = title
	}
	return nil
}
func readNCXLabels(data []byte, base string, labels map[string]string) error {
	type point struct {
		Label   string `xml:"navLabel>text"`
		Content struct {
			Src string `xml:"src,attr"`
		} `xml:"content"`
		Children []point `xml:"navPoint"`
	}
	var ncx struct {
		Points []point `xml:"navMap>navPoint"`
	}
	if err := xml.Unmarshal(data, &ncx); err != nil {
		return fmt.Errorf("EPUB NCX: %w", err)
	}
	var visit func([]point, int) error
	visit = func(points []point, depth int) error {
		if depth > 128 {
			return errors.New("EPUB navigation nesting exceeds limit")
		}
		for _, p := range points {
			if err := addEPUBLabel(labels, base, p.Content.Src, p.Label); err != nil {
				return err
			}
			if err := visit(p.Children, depth+1); err != nil {
				return err
			}
		}
		return nil
	}
	return visit(ncx.Points, 0)
}
func readNavigationLabels(data []byte, base string, labels map[string]string) error {
	decoder := epubXML(data)
	depth, tocDepth, linkDepth := 0, 0, 0
	href := ""
	var title strings.Builder
	for {
		token, err := decoder.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return fmt.Errorf("EPUB navigation: %w", err)
		}
		switch token := token.(type) {
		case xml.StartElement:
			depth++
			if depth > 256 {
				return errors.New("EPUB navigation nesting exceeds limit")
			}
			if token.Name.Local == "nav" && strings.Contains(" "+xmlAttribute(token, "type")+" ", " toc ") {
				tocDepth = depth
			}
			if tocDepth > 0 && token.Name.Local == "a" {
				linkDepth = depth
				href = xmlAttribute(token, "href")
				title.Reset()
			}
		case xml.CharData:
			if linkDepth > 0 {
				title.Write(token)
			}
		case xml.EndElement:
			if depth == linkDepth {
				if err := addEPUBLabel(labels, base, href, title.String()); err != nil {
					return err
				}
				linkDepth = 0
			}
			if depth == tocDepth {
				tocDepth = 0
			}
			depth--
		}
	}
	return nil
}

type epubTextSection struct{ Title, Text string }

func extractEPUBText(data []byte, base string, labels map[string]string) ([]epubTextSection, error) {
	decoder := epubXML(data)
	depth, bodyDepth, skipDepth, headingDepth, titleDepth := 0, 0, 0, 0, 0
	var text, heading, htmlTitle strings.Builder
	sectionTitle := labels[base]
	var sections []epubTextSection
	pendingSpace := false
	last := byte(0)
	newline := func() {
		pendingSpace = false
		if text.Len() > 0 && last != '\n' {
			text.WriteByte('\n')
			last = '\n'
		}
	}
	flush := func() {
		body := strings.TrimSpace(text.String())
		if body != "" {
			title := sectionTitle
			if title == "" {
				title = strings.TrimSpace(htmlTitle.String())
			}
			if title == "" {
				title = path.Base(base)
			}
			sections = append(sections, epubTextSection{Title: epubLabelTitle(title), Text: body})
		}
		text.Reset()
		last = 0
		pendingSpace = false
	}
	blocks := map[string]bool{"p": true, "div": true, "section": true, "article": true, "li": true, "blockquote": true, "pre": true, "tr": true, "br": true, "hr": true, "h1": true, "h2": true, "h3": true, "h4": true, "h5": true, "h6": true}
	for {
		token, err := decoder.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		switch token := token.(type) {
		case xml.StartElement:
			depth++
			if depth > 256 {
				return nil, errors.New("EPUB XHTML nesting exceeds limit")
			}
			name := strings.ToLower(token.Name.Local)
			if name == "title" && bodyDepth == 0 {
				titleDepth = depth
			}
			if name == "body" {
				bodyDepth = depth
			}
			if bodyDepth == 0 || skipDepth > 0 {
				continue
			}
			if name == "script" || name == "style" || name == "svg" || name == "noscript" || xmlAttribute(token, "hidden") != "" {
				skipDepth = depth
				continue
			}
			if id := xmlAttribute(token, "id"); id != "" {
				if label := labels[base+"#"+id]; label != "" {
					if strings.TrimSpace(text.String()) != "" {
						flush()
					}
					sectionTitle = label
				}
			}
			if blocks[name] {
				newline()
			}
			if len(name) == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && sectionTitle == "" {
				headingDepth = depth
				heading.Reset()
			}
		case xml.CharData:
			if titleDepth > 0 {
				htmlTitle.Write(token)
			}
			if bodyDepth == 0 || skipDepth > 0 {
				continue
			}
			if headingDepth > 0 {
				heading.Write(token)
			}
			for _, r := range string(token) {
				if unicode.IsSpace(r) {
					pendingSpace = true
					continue
				}
				if pendingSpace && text.Len() > 0 && last != '\n' {
					text.WriteByte(' ')
				}
				pendingSpace = false
				text.WriteRune(r)
				last = 'x'
			}
		case xml.EndElement:
			name := strings.ToLower(token.Name.Local)
			if depth == titleDepth {
				titleDepth = 0
			}
			if skipDepth == depth {
				skipDepth = 0
			} else if skipDepth == 0 && bodyDepth > 0 {
				if blocks[name] {
					newline()
				}
				if depth == headingDepth {
					sectionTitle = strings.Join(strings.Fields(heading.String()), " ")
					headingDepth = 0
				}
			}
			if depth == bodyDepth {
				bodyDepth = 0
			}
			depth--
		}
	}
	flush()
	return sections, nil
}
