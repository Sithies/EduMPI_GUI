# Synthetic large trace (performance tests)

`gen_trace.c` writes a synthetic, Score-P-like OTF2 trace of any size (e.g. 1024 ranks, 8 GB) to test
the trace views (timeline, Trace Graph, detail loading) with a memory budget smaller than the trace.
Generated traces are large: never commit them (`tools/big-trace/out/` is ignored).

## Build
Needs OTF2 (`otf2-config` in `PATH`):

```sh
# Ubuntu / Debian
sudo apt install libotf2-trace-dev otf2-tools
# or the OTF2 built for the GUI: export PATH="$HOME/otf2/bin:$PATH"   (scripts/build-otf2.sh)

cd tools/big-trace
gcc -O2 -o gen_trace gen_trace.c $(otf2-config --cflags --ldflags --libs) -lm
```

## Generate
```sh
./gen_trace --out out/stencil_1024r_8gb --ranks 1024 --ranks-per-node 32 --target-gb 8
```

| Option | Default | Meaning |
|---|---|---|
| `--out DIR` | – | output directory (gets `traces.otf2`, `traces.def`, `traces/`) |
| `--ranks N` | 1024 | MPI ranks |
| `--ranks-per-node N` | 32 | ranks per compute node (system tree) |
| `--target-gb X` | – | approximate size on disk; sets the number of iterations |
| `--iterations N` | 200 | iterations instead of `--target-gb` |
| `--seed N` | 1 | random seed (same seed = same trace) |
| `--rank-order O` | `row` | `random`: ranks placed on the grid in random order (neighbours are not rank ±1 / ±width) |

Examples (4 cores): 8 GB / 1024 ranks ~1.5 min, 0.5 GB / 256 ranks
(`--ranks 256 --target-gb 0.5`) ~2 s.

## Load in the GUI
1. Sidebar **Trace**: select *OTF2 file*, enter `.../out/stencil_1024r_8gb/traces.otf2` (or *Folder* and the directory).
2. **Memory budget (MB)**: e.g. `2048` to stay below the trace size (0 = automatic, half of the free RAM).
   Alternatively the environment variable `EDUMPI_TRACE_BUDGET_MB` overrides the setting.
3. **Load**, then enable *Trace Timeline* / *Trace Graph* under **Views** and zoom in to load details.

## Content
A simulated 2D stencil application on a periodic grid (32 x 32 for 1024 ranks):
- user regions: `main`, `timestep`, `compute_stencil`, `halo_exchange`, `global_reduce`, `row_transpose`,
  `pipeline_sweep`, `checkpoint`
- P2P: halo exchange every iteration (MPI_Irecv / MPI_Isend / MPI_Waitall, 32–128 KiB, 4 neighbours),
  ring pipeline with blocking MPI_Send / MPI_Recv every 25 iterations (late sender)
- collectives: MPI_Allreduce on WORLD every iteration, MPI_Alltoall in the row communicators (`row_comm N`)
  every 10, MPI_Bcast (WORLD) + MPI_Barrier per node (`node_comm N`) every 50
- load imbalance: gradient over the grid, every 7th node slower, single hotspot ranks, OS noise
- causally consistent times: receives / Waitall never end before the message arrives, collectives end after
  the last rank entered, so the waiting times are real
- definitions like Score-P: system tree machine → node → "MPI Rank N", COMM_LOCATIONS / COMM_GROUP groups,
  named communicators, exact event counts per location
