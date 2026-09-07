// Open publishing example: a terminal-only app with no device or network writes.
package main

import (
	"fmt"
	"os"
)

const version = "0.1.0"

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--version" {
		fmt.Println("open-publish-demo", version)
		return
	}
	fmt.Println("C1 Open Publishing Demo", version)
	fmt.Println("This app was published without a developer token.")
	fmt.Println("The repository signs packages; the public key verifies them.")
	fmt.Println("No device settings or user files have been changed.")
}
