# Build and test tools

Scripts added here must:
- target FreeBSD 15.1 amd64 unless explicitly parameterized;
- run in the foreground (no nohup/background builds);
- stop on errors and print the exact source/object/kernel paths;
- install experimental kernels to a separate directory;
- never overwrite the known-good kernel without explicit user action.

Add scripts only after testing them on the target machine.
