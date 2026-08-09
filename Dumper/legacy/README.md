# Legacy in-process transport archive

`Dumper/API` and `Dumper/Server` are retained in their original paths as historical
source because repository policy prohibits deleting files. They are not compiled by
`Dumper/UExplorerCore.vcxproj`, are not referenced by `Dumper/Main.cpp`, and are not a
runtime compatibility path.

The release desktop boundary is:

```text
React -> Tauri command/channel -> Rust DomainService -> PID-scoped Named Pipe -> Core
```

Do not add these sources back to the Core project. If an external HTTP gateway becomes
a release requirement, implement it in the Rust Host over the same DomainService and
give it an explicit opt-in configuration and contract suite.
