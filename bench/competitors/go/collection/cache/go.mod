module collection-cache

go 1.22

require (
	collgen v0.0.0
	github.com/dgraph-io/ristretto/v2 v2.1.0
)

require (
	github.com/cespare/xxhash/v2 v2.3.0 // indirect
	github.com/dustin/go-humanize v1.0.1 // indirect
	github.com/pkg/errors v0.9.1 // indirect
	golang.org/x/sys v0.29.0 // indirect
)

replace collgen => ../common
