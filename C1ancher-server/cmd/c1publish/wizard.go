package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// With no arguments, provide a console workflow usable by double-clicking the
// Windows EXE. CLI mode remains scriptable and never prompts unexpectedly.
func wizard(input io.Reader, output io.Writer) error {
	base, err := os.UserConfigDir()
	if err != nil {
		return err
	}
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	directory := filepath.Dir(executable)
	origin, trustKey, err := loadPublicConfig(directory, "", "")
	if err != nil {
		return err
	}
	defaultToken := filepath.Join(base, "C1ancher", "publisher-tokens", "publisher.token")
	return wizardWithRunner(input, output, defaultToken, origin, func(args []string) error {
		if len(trustKey) != 0 {
			// Pin the bundled-key requirement even though the wizard passes an
			// explicit origin. A removed key must fail closed, not become optional.
			args = append(args, "-public-key", filepath.Join(directory, "repository.ed25519.pub"))
		}
		return runArgsWithOutput(args, directory, output)
	})
}

// Inject the command runner to test every prompt without contacting a server.
func wizardWithRunner(input io.Reader, output io.Writer, defaultToken, origin string, run func([]string) error) error {
	r := bufio.NewReader(input)
	ask := func(prompt, fallback string) (string, error) {
		if fallback != "" {
			fmt.Fprintf(output, "%s [%s]: ", prompt, fallback)
		} else {
			fmt.Fprintf(output, "%s: ", prompt)
		}
		line, err := r.ReadString('\n')
		if err != nil && !(errors.Is(err, io.EOF) && len(line) > 0) {
			return "", err
		}
		line = strings.TrimSpace(line)
		if line == "" {
			line = fallback
		}
		// Accept Windows Explorer's copy-as-path result without shell parsing.
		if len(line) >= 2 && line[0] == '"' && line[len(line)-1] == '"' {
			line = line[1 : len(line)-1]
		}
		return line, nil
	}
	fmt.Fprintf(output, "C1-Slim Publisher %s / 应用发布器\n", publisherVersion)
	fmt.Fprintf(output, "服务器 / Server: %s\n", origin)
	fmt.Fprintln(output, "自助注册作者后，用同一个令牌发布和更新自己的应用，无需管理员审批。")
	fmt.Fprintln(output, "本工具负责打包上传；请先编译设备用 MIPS 应用，不能上传本发布器 EXE。")
	for {
		fmt.Fprintln(output, "\n1. 注册作者并保存令牌 / 重试注册\n2. 使用已有令牌发布应用\n3. 查询应用建议版本\n0. 退出")
		choice, err := ask("请选择", "0")
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return err
		}
		if choice == "0" {
			return nil
		}
		if choice != "1" && choice != "2" && choice != "3" {
			fmt.Fprintln(output, "请输入 0、1、2 或 3。")
			continue
		}
		args := []string{"-server", origin}
		if choice == "1" || choice == "2" {
			if strings.HasPrefix(origin, "http://") {
				fmt.Fprintln(output, "注意：该服务器使用 HTTP，令牌及上传内容可能被截获或篡改。")
				yes, e := ask("接受该风险并继续？输入 YES", "NO")
				if e != nil {
					return e
				}
				if yes != "YES" {
					continue
				}
				args = append(args, "-allow-insecure-http")
			}
			token, e := ask("令牌文件路径（不要输入令牌内容）", defaultToken)
			if e != nil {
				return e
			}
			if choice == "1" && token == defaultToken {
				if e = os.MkdirAll(filepath.Dir(token), 0700); e != nil {
					fmt.Fprintln(output, "无法创建令牌目录：", e)
					continue
				}
			}
			args = append(args, "-token-file", token)
		}
		if choice == "1" {
			author, e := ask("作者名（名称唯一，不代表已核验的真实身份）", "")
			if e != nil {
				return e
			}
			args = append(args, "-register", "-author", author)
		} else {
			id, e := ask("应用 ID（英文、数字、点、下划线或短横线；更新时必须保持不变）", "")
			if e != nil {
				return e
			}
			args = append(args, "-id", id)
			if choice == "3" {
				args = append(args, "-next-version")
			} else {
				name, e := ask("应用显示名称", "")
				if e != nil {
					return e
				}
				version, e := ask("本次版本（须与已编译程序一致；更新必须升版本）", "")
				if e != nil {
					return e
				}
				kind, e := ask("1=单文件程序，2=含资源的 payload 目录", "1")
				if e != nil {
					return e
				}
				args = append(args, "-name", name, "-version", version)
				switch kind {
				case "1":
					file, e := ask("设备程序文件路径（不是发布器 EXE）", "")
					if e != nil {
						return e
					}
					args = append(args, "-binary", file)
				case "2":
					folder, e := ask("payload 目录路径（只放程序和资源，不要包含令牌）", "")
					if e != nil {
						return e
					}
					entry, e := ask("入口相对路径，例如 bin/my-app", "")
					if e != nil {
						return e
					}
					args = append(args, "-payload", folder, "-entry", entry)
				default:
					fmt.Fprintln(output, "请选择 1 或 2；尚未上传。")
					continue
				}
				yes, e := ask("确认发布到公共应用仓库？输入 YES", "NO")
				if e != nil {
					return e
				}
				if yes != "YES" {
					continue
				}
			}
		}
		if err = run(args); err != nil {
			fmt.Fprintln(output, "操作失败：", err)
		} else {
			fmt.Fprintln(output, "操作成功。请妥善备份令牌文件，不要公开或放进上传目录。")
		}
	}
}
