// Self-service publishing example: terminal output only, no file/device/network writes.
package main

import (
	"fmt"
	"os"
)

// Set with go build -ldflags=-X=main.version=0.1.1 for an updated release.
var version = "0.1.0"

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--version" {
		fmt.Println("self-service-demo", version)
		return
	}
	fmt.Println("C1 Self-service Publishing Demo", version)
	fmt.Println("Registered authors can publish their own new applications.")
	fmt.Println("Only their publisher token can update an owned application.")
	fmt.Println("This example only prints text; no settings or user files are changed.")
}
