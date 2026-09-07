package main

import (
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"unicode/utf8"
)

func TestWizardRegisterPublishAndNextVersion(t *testing.T) {
	token := filepath.Join(t.TempDir(), "private", "publisher.token")
	input := strings.Join([]string{
		"1", "", "测试作者",
		"2", "", "own-app", "中文应用", "0.1.0", "1", `"C:\device apps\app"`, "YES",
		"3", "own-app", "0",
	}, "\n") // Final input deliberately has no newline.
	var out bytes.Buffer
	var calls [][]string
	err := wizardWithRunner(strings.NewReader(input), &out, token, "https://repository.invalid", func(args []string) error {
		calls = append(calls, append([]string(nil), args...))
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	want := [][]string{
		{"-server", "https://repository.invalid", "-token-file", token, "-register", "-author", "测试作者"},
		{"-server", "https://repository.invalid", "-token-file", token, "-id", "own-app", "-name", "中文应用", "-version", "0.1.0", "-binary", `C:\device apps\app`},
		{"-server", "https://repository.invalid", "-id", "own-app", "-next-version"},
	}
	if !reflect.DeepEqual(calls, want) {
		t.Fatalf("unexpected wizard commands: %#v", calls)
	}
	if info, err := os.Stat(filepath.Dir(token)); err != nil || !info.IsDir() {
		t.Fatal("default private directory was not created")
	}
	if !utf8.Valid(out.Bytes()) || !strings.Contains(out.String(), "应用发布器") || !strings.Contains(out.String(), "https://repository.invalid") || strings.Contains(out.String(), "接受该风险") {
		t.Fatal("wizard encoding or HTTPS consent is incorrect")
	}
}

func TestWizardRegistrationRetryKeepsAuthorAndPath(t *testing.T) {
	token := filepath.Join(t.TempDir(), "publisher.token")
	input := "1\nYES\n\nAuthor\n1\nYES\n\nAuthor\n0\n"
	var out bytes.Buffer
	var calls [][]string
	err := wizardWithRunner(strings.NewReader(input), &out, token, "http://repository.invalid", func(args []string) error {
		calls = append(calls, append([]string(nil), args...))
		if len(calls) == 1 {
			return errors.New("registration response unavailable; token file retained")
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(calls) != 2 || !reflect.DeepEqual(calls[0], calls[1]) || !strings.Contains(strings.Join(calls[0], " "), "-allow-insecure-http") {
		t.Fatal("registration retry changed identity or failed to require HTTP consent")
	}
	if !strings.Contains(out.String(), "操作失败") || !strings.Contains(out.String(), "操作成功") {
		t.Fatal("wizard did not remain usable after a failure")
	}
}

func TestWizardPayloadFlow(t *testing.T) {
	var calls [][]string
	err := wizardWithRunner(strings.NewReader("2\nprivate.token\nown-app\nApp\n0.1.1\n2\n\"C:\\payload dir\"\nbin/app\nYES\n0\n"), io.Discard, "unused", "https://repository.invalid", func(args []string) error {
		calls = append(calls, args)
		return nil
	})
	want := []string{"-server", "https://repository.invalid", "-token-file", "private.token", "-id", "own-app", "-name", "App", "-version", "0.1.1", "-payload", `C:\payload dir`, "-entry", "bin/app"}
	if err != nil || len(calls) != 1 || !reflect.DeepEqual(calls[0], want) {
		t.Fatalf("payload flow: %#v, %v", calls, err)
	}
}

func TestWizardCancellationAndIncompleteInputSendNothing(t *testing.T) {
	for _, test := range []struct {
		name, origin, input string
		eof                 bool
	}{
		{"exit", "http://repository.invalid", "0", false},
		{"menu EOF", "http://repository.invalid", "", false},
		{"invalid choice", "http://repository.invalid", "9\n0\n", false},
		{"HTTP declined", "http://repository.invalid", "1\nNO\n0\n", false},
		{"publish declined", "https://repository.invalid", "2\nprivate.token\nown-app\nApp\n0.1.0\n1\napp\nNO\n0\n", false},
		{"invalid payload kind", "https://repository.invalid", "2\nprivate.token\nown-app\nApp\n0.1.0\n9\n0\n", false},
		{"incomplete register", "https://repository.invalid", "1\nprivate.token\n", true},
	} {
		t.Run(test.name, func(t *testing.T) {
			calls := 0
			err := wizardWithRunner(strings.NewReader(test.input), io.Discard, filepath.Join(t.TempDir(), "publisher.token"), test.origin, func([]string) error { calls++; return nil })
			if calls != 0 || (test.eof && !errors.Is(err, io.EOF)) || (!test.eof && err != nil) {
				t.Fatalf("cancelled input ran a command or returned wrong error: %d, %v", calls, err)
			}
		})
	}
}
