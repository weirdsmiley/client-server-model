# Client-Server model

This is a multi-threaded concurrent TCP-based implementation for client-server
model. The connection is requested by the client, which asks the server for `N`
top CPU consuming processes, and server serves this request. Upon receiving the
data from the server, client returns back its own top CPU consuming process to
the server, which is simply printed onto the standard output.

## Build
In the root of the project, run

```bash
$ make
```

This will create a build/ directory in the root of the project, which will
contain two binaries `srv` (for the server) and `cli` (for the client). These
binaries are independant and can be run on different machines (assuming, they
are build for those architectures).
