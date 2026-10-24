# Command Entry Point

## External Interface

`cisda-sim WORKLOAD.json` accepts exactly one workload manifest and writes one
simulation-result JSON document to standard output. Invalid arguments return
64; workload or simulation failures return 2.

## Internal Mechanism

The entry point constructs `Simulator`, runs the manifest, and serializes the
result. It exposes no secondary runtime commands.
