# cajeta.io

This section measures buffers, files and sockets. Network rows run over loopback with the peer in-process.

| Page | |
|---|---|
| [Buffer](Buffer.md) | The byte buffer behind every reader and writer |
| [File](file/File.md) | Whole-file and positioned reads and writes |
| [FileInfo](file/FileInfo.md) | File metadata |
| [FileReader](file/FileReader.md) | Buffered line and token reading |
| [FileWriter](file/FileWriter.md) | Buffered writing |
| [Path](file/Path.md) | Path joining and normalization |
| [Watcher](file/Watcher.md) | Filesystem change notification |
| [Directory walk](file/Directory.md) | Walking a tree |
| [TcpStream](net/TcpStream.md) | A connected TCP socket |
| [TcpListener](net/TcpListener.md) | Accepting connections |
| [UdpSocket](net/UdpSocket.md) | Datagrams |
| [Server](net/Server.md) | The reactor-driven server |
| [ByteBuffer](net/ByteBuffer.md) | The network byte buffer with big-endian accessors |
| [BufferPool](net/BufferPool.md) | Pooled buffers for the reactor |
| [Uri](net/uri/Uri.md) | URI parsing |
| [UriBuilder](net/uri/UriBuilder.md) | URI construction |

## Standing

Geometric mean 2.5x behind over 20 benchmarks, 3 wins and 17 losses. Where the kernel sets the pace, Cajeta is at parity: sequential file read and write, bulk TCP, UDP send, a thousand idle connections, and buffer fill; reading lines is a 4x win. The losses are echo throughput (3.4x) and latency (4x), the URI parser and builder (16x), slices (11x) and the line writer (8x), none yet investigated.
