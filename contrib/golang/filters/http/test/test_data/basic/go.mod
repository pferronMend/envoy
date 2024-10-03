module example.com/basic

go 1.18

require github.com/envoyproxy/envoy v1.28.7

require google.golang.org/protobuf v1.30.0 // indirect

replace github.com/envoyproxy/envoy => ../../../../../../../
