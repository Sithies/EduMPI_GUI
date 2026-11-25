# EduMPI GUI – Frontend 

**EduMPI GUI** is the graphical interface for [EduMPI](https://github.com/AnnaLena77/EduMPI),  
an Open MPI fork with integrated near-real-time performance measurement.  

The GUI provides students and educators with a **simple, intuitive way to run MPI applications on HPC clusters**  
and to **visualize communication patterns, performance metrics, and wait states** without prior knowledge of SSH, SLURM, or traditional profiling tools.

---

## ✨ Key Features

- **Simplified Cluster Access**
  - No SSH or SLURM scripting required  
  - Direct login via GUI  
  - Automatic file upload and job submission  

- **Job Management**
  - Select MPI executable and number of processes  
  - Start, monitor, and abort jobs interactively  
  - View job status in real-time  

- **Integrated Visualization**
  - **2D process view**: communication, bytes sent/received, wait times  
  - **3D cluster view**: processes as colored cubes on their physical nodes  
  - **Communication matrix**: direct process-to-process message analysis  
  - Interactive timeline navigation (per-second or interval-based)  

- **Trace Analysis (OTF2)** – see [Trace Analysis](#-trace-analysis-otf2)
  - Load an OTF2 trace (e.g. from Score-P) from a local file, no database or cluster needed  
  - Replay it in the 2D, 3D and matrix views in a separate trace window  
  - **Trace Timeline**: one row per rank, every MPI call as a bar, zoom from the whole run down to 10 µs  
  - Late-sender wait times, message arrows and per-call details  
  - **Trace Graph**: who communicates with whom in the current window, ranks as nodes, messages as edges  

- **Educational Focus**
  - Helps students understand MPI behavior through visual cues  
  - Removes barriers of complex HPC toolchains  
  - Designed for classroom and lab exercises  

---

## ⚙️ Architecture

The GUI acts as the **frontend client** in the EduMPI ecosystem:

1. Students launch the GUI (AppImage or local build)  
2. Login to the HPC cluster with provided credentials  
3. Select an MPI program and execution parameters  
4. EduMPI (cluster-side) executes the job and writes performance data to **TimescaleDB**  
5. The GUI queries aggregated data in near-real-time and visualizes results  

![GUI Workflow](docs/architecture/pictogram.png)

Traces take a second path that needs neither cluster nor database: the GUI reads an OTF2 trace from
a local file in one pass and feeds both the existing per-second views and the Trace Timeline from it.

```
traces.otf2
    │
    ▼
OTF2 reader (trace/otf2_trace_reader)
    ├──► TraceAggregator   per-second sums   ──┐
    └──► TimelineBuilder   timeline store    ──┤
                                               ▼
                   Trace_Data_Source (own thread, memory budget)
                        ├──► 2D / 3D / matrix   (same signals as the database)
                        └──► Trace Timeline / Trace Graph
```

Everything up to `Trace_Data_Source` lives in the library `trace/` (`edumpi_trace`), which does not
depend on Qt Quick and is tested on its own. Only the reader knows OTF2; the rest works on the GUI's
own records, so traces written later by EduMPI itself need no changes beyond the reader.

---

## 🖥️ Screenshots


  ![Screenshot](docs/screenshots/gui.png)

---

## 🚀 Installation

### Option 1: Download Prebuilt AppImage (Recommended)
1. Download the [EduMPI GUI AppImage](https://github.com/AnnaLena77/EduMPI_GUI_Download)  
2. Make it executable:  
   ```bash
   chmod +x EduMPI_GUI-x86_64.AppImage
   ```
3. run it:
   ```bash
    ./EduMPI_GUI-x86_64.AppImage
    ```

### Option 2: Build from Source
1. Clone the repository:
     ```bash
    git clone git@github.com:AnnaLena77/EduMPI_GUI.git
    cd EduMPI_GUI
    ```
2. Install dependencies: Qt 6.12 (Quick3D, ShaderTools, Sql)
3. Build using CMake
4. Run the resulting binary

Trace support is built automatically when OTF2 is installed, otherwise it is left out; see
[Building with trace support](#building-with-trace-support).

### Dev mode (local dev cluster)
For development against the local container cluster in the EduMPI repo (`dev/`), configure with
`-DEDUMPI_DEV_MODE=ON` (never used for release builds) and start the GUI from a shell where
`source <EduMPI>/dev/ssh/activate` was run. The GUI then reads the connection values from
`<EduMPI>/dev/gui.env`, connects to the database and the cluster automatically, reconnects every 5 s
after a restart of the dev cluster, and shows "[DEV]" in the window title. Without the flag or without
`EDUMPI_DEV_ENV` the GUI behaves as before. Details: `dev/README.md` in the EduMPI repo.

---

## 🔍 Trace Analysis (OTF2)

A trace records every MPI call with its start, end and partner. Where the 2D/3D views show sums per
second ("how much?"), a trace shows *when, with whom and in which order* ranks communicate, and where
they wait.

### Creating a trace
A trace is written by [Score-P](https://www.vi-hps.org/projects/score-p/) during a run. Each run creates
an *experiment folder*: the directory Score-P writes all results of that run to. For tracing it holds
`traces.otf2` (the entry file), `traces.def` and a `traces/` subfolder with one file per process, next to
the profile files (`profile.cubex`, `summary.cubex`). The GUI needs the whole folder, or at least
`traces.otf2`, `traces.def` and `traces/` together; `traces.otf2` alone is not enough.

Two ways to get one:

- **From the GUI**: menu **Non-Visualization → MPI Program with Score-P**. The GUI compiles the program
  with `scorep mpicc`, runs it on the cluster with `scalasca -analyze -t` (tracing on) and copies the
  experiment folder `scorep_<program>_<processes>_<job id>` into your home directory.
- **By hand**: build with `scorep mpicc …` and run with tracing enabled, e.g.
  `SCOREP_ENABLE_TRACING=true mpiexec -n 4 ./program` or `scalasca -analyze -t mpiexec -n 4 ./program`.

Functions excluded in `scorep.filter` are not in the trace and cannot be shown.

### Loading a trace
1. In the main window's sidebar, open **Trace**.
2. Choose **OTF2 file** (`traces.otf2`) or **Folder** (the experiment folder), then **Browse**.
3. Optionally set the **memory budget** (see [Memory budget](#memory-budget)).
4. Click **Load**. A separate trace window opens; the main window stays free for live runs.

### Trace window
- **Views**: 2D, 3D and matrix work as for a database run, with the bottom bar replaying second by
  second. **Trace Timeline** and **Trace Graph** are only available here.
- **Trace Timeline**: one row per rank, grouped by node, one bar per MPI call.

  | Colour | Meaning |
  | --- | --- |
  | green | point-to-point send (`MPI_Send`, `MPI_Isend`, …) |
  | red | point-to-point receive and completion (`MPI_Recv`, `MPI_Wait*`, …) |
  | blue | collective (`MPI_Bcast`, `MPI_Allreduce`, …) |
  | grey | other MPI calls |
  | yellow hatching | waiting for a late sender inside a receive |

  - **Zoom**: mouse wheel or two-finger swipe up/down on a trackpad, around the pointer, or the
    **+** / **−** buttons; from the whole run down to 10 µs, the time axis switches between s, ms and µs.
    **Full run** shows everything again.
  - **Move in time**: Shift + mouse wheel, swipe sideways on a trackpad, or drag the timeline with
    the mouse. The overview bar above the timeline shows the whole run; click or drag it to move the
    visible window.
  - **Scroll the rows**: Alt + mouse wheel, drag the timeline up or down, or the scroll bar on the right.
  - **Messages**: click a rank's name to select it; arrows show its messages, other ranks are dimmed.
  - **Phases**: a band per rank shows the user function running at that time (outermost or innermost
    level).
  - **Details**: click a bar to see function, rank, start, duration, wait time, partner, bytes, tag,
    communicator and call path in **Trace Options**. For `MPI_Wait*`, *Completes* names the
    nonblocking receive it completes, e.g. `MPI_Irecv from rank 3`.
- **Trace Graph**: communication graph of the current time window (the timeline's window; the whole
  run until one is chosen). Its own view under **Views**, or below the timeline (**Graph** button in
  the timeline toolbar, divider movable).
  - Ranks are nodes on a circle (or a grid), grouped and coloured by compute node; P2P messages are
    edges (arrows from sender to receiver), collectives dashed edges to one diamond per communicator.
  - Edge width: bytes (default), number of messages or waiting time; edge colour: waiting time of the
    receiver (late sender), grey = no waiting, red = most waiting in the window.
  - A message counts when its send call starts in the window, a collective when the rank's call
    starts in it (as in *Show window in 3D/2D/Matrix*).
  - Many ranks: only the heaviest edges are drawn (100 / 500 / 2000 / all), the selected rank's edges
    always. **Compute nodes** merges the ranks of each node into one graph node.
  - Click a rank to select it (also in the timeline, other ranks are dimmed); click an edge to see
    its bytes, messages and waiting time in **Trace Options**.
  - Long windows of large traces: the graph needs the window's messages; as in the timeline, zoom in
    until they can be loaded within the memory budget.
- **Trace Options** (sidebar of the trace window): display switches of the timeline (arrows, all arrows,
  highlight waiting, phases) and of the graph (below the timeline, P2P, collectives, direction, edge
  width, ranks / compute nodes, layout, number of edges), details of the clicked bar and edge, trace
  info (name, path, ranks, duration, unmatched messages), and **Show window in 3D/2D/Matrix** to hand
  the visible time window to the other views.

### Memory budget
The timeline never uses more memory than its budget. By default this is half of the free memory. Set
it in the **Trace** panel before clicking **Load** (MB, 0 = automatic; it is remembered for later loads)
or with the environment variable `EDUMPI_TRACE_BUDGET_MB`, which takes precedence. A trace that fits
is loaded completely. A larger trace is loaded as a condensed overview, and details for the visible
window are read in the background when you zoom in.
If even a short window of a very dense phase does not fit, the timeline shows *Too dense for the
memory budget — zoom in further*.

### Building with trace support
Trace support is optional. It needs OTF2, which is linked statically into the GUI at build time, so
users of a finished build need neither OTF2 nor Score-P. CMake decides with the option
`EDUMPI_TRACE`:

| `EDUMPI_TRACE` | Behaviour |
| --- | --- |
| `AUTO` (default) | Trace support is built if OTF2 is found in `OTF2_ROOT`, otherwise the GUI builds without it. The configure output says which (`Trace support: on` / `off`). |
| `ON` | OTF2 is required; configuring fails if it is missing. Used by the CI and release builds. |
| `OFF` | No trace support, even if OTF2 is installed. |

1. Install OTF2 once into a directory of your choice, the *OTF2 prefix* (any path you can write to,
   e.g. `/opt/otf2` or a folder in your home directory):
    ```bash
    scripts/build-otf2.sh <otf2-prefix>
    ```
   The script downloads the OTF2 3.2 sources (the version Score-P 10.1 uses) from the Jülich
   Supercomputing Centre, checks the archive's checksum, builds OTF2 as a static library and installs
   it into `<otf2-prefix>`. It needs `curl`, a C compiler and `make`, takes a few minutes, and does
   nothing if `<otf2-prefix>/bin/otf2-config` already exists. An OTF2 you installed differently works
   as well if `<otf2-prefix>/bin/otf2-config` exists; for a self-contained GUI it must include the
   static library.
2. Configure and build the GUI, pointing CMake at the same prefix:
    ```bash
    cmake -S . -B build -DOTF2_ROOT=<otf2-prefix>
    cmake --build build
    ```
    Without `-DOTF2_ROOT`, CMake looks in `~/.local/otf2`. Add `-DEDUMPI_TRACE=ON` to make a missing
    OTF2 an error instead of a silent build without traces. If you install OTF2 after a first build,
    run CMake again (e.g. "Run CMake" in Qt Creator) so it finds it.
3. Optional, run the trace tests:
    ```bash
    cmake --build build --target trace_tests && ctest --test-dir build -R trace
    ```

Without trace support the GUI builds and runs as before; loading a trace then shows a short notice. Windows builds never have trace support, because OTF2 does not officially
support Windows. Use Qt 6.12 or newer: Qt Quick 3D 6.11.0 can crash at random when a 3D view is
closed or replaced, which happens often when switching views in the trace window.

## 📚 References

EduMPI is described in:

- Roth, Anna-Lena; Süß, Tim (2023): *Performance Analysis Tools for MPI Applications and their Use in Programming Education*. In Companion of the 2023 ACM/SPEC International Conference on Performance Engineering (ICPE '23 Companion). Association for Computing Machinery, New York, NY, USA, 361–368. 
DOI: [10.1145/3578245.3584358](https://doi.org/10.1145/3578245.3584358) 
- Roth, Anna-Lena; James, David; Kuhn, Michael; Konert, Johannes (2024): *Enhancing Parallel Programming Education with High-Performance Clusters Utilizing Performance Analysis*. Proceedings of DELFI 2024. DOI: 10.18420/delfi2024_42. Gesellschaft für Informatik e.V. 
DOI: [10.18420/delfi2024_42](https://doi.org/10.18420/delfi2024_42)
- Roth, Anna-Lena; James, David; Kuhn, Michael (2025): *EduMPI – Simplifying the Use of High-Performance Clusters and Focusing Performance Analysis in Parallel Programming Education*. PARS-Mitteilungen: Vol. 37. Gesellschaft für Informatik e.V., Fachgruppe PARS. ISSN: 0177-0454Link: [LINK](https://dl.gi.de/server/api/core/bitstreams/5ee764b8-731a-4b8e-aca4-28cb0538fd7b/content)
- Roth, Anna-Lena; James, David; Kuhn, Michael; Frisch, Dustin (2025): *Making MPI Collective Operations Visible: Understanding Their Utility and Algorithmic insights*. In: Nagel, W.E., Goehringer, D., Diniz, P.C. (eds) Euro-Par 2025: Parallel Processing. Euro-Par 2025. Lecture Notes in Computer Science, vol 15900. Springer, Cham. 
DOI: [10.1007/978-3-031-99854-6_5](https://doi.org/10.1007/978-3-031-99854-6_5)

---

## 🔮 Future Work

- Improve ring buffer adaptivity to avoid overflow in micro-benchmarks  
- Extend support for additional MPI language bindings beyond C  
- Portable integration as an **independent framework** instead of Open MPI fork  
- Additional visualization dashboards (historical runs, anomaly detection)  

---



