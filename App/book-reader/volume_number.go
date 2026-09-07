package main

import (
	"regexp"
	"strconv"
	"strings"
)

const volumeNumberChars = `0-9０-９零〇一二三四五六七八九十百千两兩壹贰貳叁參肆伍陆陸柒捌玖拾佰仟`

var numberedVolumePattern = regexp.MustCompile(`^第\s*([` + volumeNumberChars + `]+)\s*([卷部篇])(?:[：:\s　].*)?$`)
var reverseNumberedVolumePattern = regexp.MustCompile(`^([卷部篇])\s*([` + volumeNumberChars + `]+)(?:[：:\s　].*)?$`)

func volumeNumber(title string) (number int, unit string, ok bool) {
	if match := numberedVolumePattern.FindStringSubmatch(strings.TrimSpace(title)); match != nil {
		number, ok = parseVolumeNumber(match[1])
		return number, match[2], ok
	}
	if match := reverseNumberedVolumePattern.FindStringSubmatch(strings.TrimSpace(title)); match != nil {
		number, ok = parseVolumeNumber(match[2])
		return number, match[1], ok
	}
	return 0, "", false
}

func parseVolumeNumber(text string) (int, bool) {
	text = strings.NewReplacer("壹", "一", "贰", "二", "貳", "二", "叁", "三", "參", "三",
		"肆", "四", "伍", "五", "陆", "六", "陸", "六", "柒", "七", "捌", "八", "玖", "九",
		"拾", "十", "佰", "百", "仟", "千", "两", "二", "兩", "二", "〇", "零").Replace(text)
	var digits strings.Builder
	for _, character := range text {
		switch {
		case character >= '0' && character <= '9':
			digits.WriteRune(character)
		case character >= '０' && character <= '９':
			digits.WriteRune('0' + character - '０')
		default:
			if index := strings.IndexRune("零一二三四五六七八九", character); index >= 0 {
				// Each of these Chinese digits is three UTF-8 bytes.
				digits.WriteByte(byte('0' + index/3))
			} else {
				digits.WriteRune(character)
			}
		}
	}
	if value, err := strconv.Atoi(digits.String()); err == nil {
		return value, value >= 1 && value <= 9999
	}
	// Unit-form Chinese numbers must be canonical, e.g. 一百零二. Reject
	// ambiguous abbreviations (一百二) and malformed repeated/ascending units.
	value, pending, previousUnit := 0, 0, 10000
	for _, character := range text {
		if index := strings.IndexRune("零一二三四五六七八九", character); index >= 0 {
			pending = index / 3
			continue
		}
		unit := map[rune]int{'十': 10, '百': 100, '千': 1000}[character]
		if unit == 0 || unit >= previousUnit {
			return 0, false
		}
		if pending == 0 {
			if value == 0 && unit == 10 {
				pending = 1
			} else {
				return 0, false
			}
		}
		value += pending * unit
		pending, previousUnit = 0, unit
	}
	value += pending
	if value < 1 || value > 9999 {
		return 0, false
	}
	canonical := chineseNumber(value)
	return value, text == canonical || (value >= 10 && value < 20 && text == "一"+canonical)
}

func chineseNumber(number int) string {
	digits := []rune("零一二三四五六七八九")
	units := []string{"", "十", "百", "千"}
	result, zero := "", false
	for position, divisor := 3, 1000; divisor > 0; position, divisor = position-1, divisor/10 {
		digit := number / divisor % 10
		if digit == 0 {
			if result != "" {
				zero = true
			}
			continue
		}
		if zero {
			result += "零"
			zero = false
		}
		if !(divisor == 10 && digit == 1 && result == "") {
			result += string(digits[digit])
		}
		result += units[position]
	}
	return result
}

// Only a proven sequence 1,2,... can be presented as a hierarchy. Failure
// clears UI ownership, but never changes raw chapter IDs, ranges or body data.
func (document *Document) validateVolumeSequence(outline *chapterOutline) {
	last, kind := 0, ""
	valid := outline.volumes > 0
	for index, entry := range outline.entries {
		if !entry.volume {
			continue
		}
		number, unit, ok := volumeNumber(document.Chapters[index].Title)
		if !ok {
			valid = false
			break
		}
		if entry.parent >= 0 {
			continue
		} // Same-title repetitions already merged.
		if number != last+1 || (kind != "" && unit != kind) {
			valid = false
			break
		}
		last, kind = number, unit
	}
	outline.grouped = valid
	if !valid {
		outline.volumes = 0
		for index := range outline.entries {
			outline.entries[index].parent = -1
			outline.entries[index].chapterCount = 0
		}
	}
}
